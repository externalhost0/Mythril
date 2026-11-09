#include "vkutil.h"
#include "vk_sync_tables.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace mythril;
namespace st = mythril::sync_tables;

namespace {
	template <size_t N>
	uint64_t expand(const st::Entry (&table)[N], uint64_t mask) {
		uint64_t out = 0;
		for (const st::Entry& e : table)
			if (mask & e.bit)
				out |= e.mask;
		return out;
	}
	uint64_t expandStages(VkPipelineStageFlags2 mask) { return expand(st::kStageExpansion, mask); }
	uint64_t expandAccess(VkAccessFlags2 mask) { return expand(st::kAccessExpansion, mask); }
	bool contains(uint64_t outer, uint64_t inner) { return (inner & ~outer) == 0; }

	// AGENTS: oracle side, write access per the spec's access flag descriptions, never from vkutil::kWriteAccess
	constexpr VkAccessFlags2 kSpecBufferWrites = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
	bool writes(StageAccess a) { return (expandAccess(a.access) & expandAccess(kSpecBufferWrites)) != 0; }

	using TrackFn = std::optional<vkutil::BufferBarrier> (*)(vkutil::BufferAccessTracker&, StageAccess);

	std::vector<StageAccess> everyRequest() {
		constexpr BufferAccess kAccesses[] = {BufferAccess::ShaderRead, BufferAccess::ShaderWrite, BufferAccess::ShaderReadWrite, BufferAccess::IndexRead,
		                                      BufferAccess::IndirectRead, BufferAccess::TransferRead, BufferAccess::TransferWrite};
		constexpr PassDesc::Type kPassTypes[] = {PassDesc::Type::Graphics, PassDesc::Type::Compute, PassDesc::Type::Intermediate, PassDesc::Type::RayTracing};
		std::vector<StageAccess> out;
		for (BufferAccess access : kAccesses)
			for (PassDesc::Type type : kPassTypes) {
				const StageAccess a = vkutil::GetBufferStageAccess(access, type);
				bool seen = false;
				for (const StageAccess& b : out)
					seen |= b.stage == a.stage && b.access == a.access;
				if (!seen)
					out.push_back(a);
			}
		return out;
	}

	// spec: a hazard pair is ordered when a chain of barriers runs from i to j, the first covering i in its src scope (and its writes) and the last covering j in its dst scope (and j's access when i wrote)
	std::vector<std::string> check(TrackFn track, const std::vector<StageAccess>& seq) {
		vkutil::BufferAccessTracker tracker = {};
		std::vector<std::optional<vkutil::BufferBarrier>> barriers;
		for (const StageAccess& a : seq)
			barriers.push_back(track(tracker, a));

		std::vector<std::string> failures;
		for (size_t k = 0; k < seq.size(); ++k) {
			if (!barriers[k])
				continue;
			const vkutil::BufferBarrier& b = *barriers[k];
			if ((b.src.stage | b.dst.stage) & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || (b.src.access | b.dst.access) & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT))
				failures.push_back("access " + std::to_string(k) + ": ALL_COMMANDS/MEMORY_READ/MEMORY_WRITE is banned");
			uint64_t hazardStages = 0, priorWrites = 0;
			for (size_t i = 0; i < k; ++i) {
				if (writes(seq[i]) || writes(seq[k]))
					hazardStages |= expandStages(seq[i].stage);
				if (writes(seq[i]))
					priorWrites |= expandAccess(seq[i].access & kSpecBufferWrites);
			}
			if (!contains(hazardStages, expandStages(b.src.stage)) || !contains(priorWrites, expandAccess(b.src.access)))
				failures.push_back("access " + std::to_string(k) + ": src not justified by an earlier hazard");
			if (!contains(expandStages(seq[k].stage), expandStages(b.dst.stage)) || !contains(expandAccess(seq[k].access), expandAccess(b.dst.access)))
				failures.push_back("access " + std::to_string(k) + ": dst wider than the request");
		}
		for (size_t j = 0; j < seq.size(); ++j) {
			for (size_t i = 0; i < j; ++i) {
				if (!writes(seq[i]) && !writes(seq[j]))
					continue;
				std::vector<bool> reached(seq.size(), false);
				bool ordered = false;
				for (size_t k = i + 1; k <= j && !ordered; ++k) {
					if (!barriers[k])
						continue;
					const vkutil::BufferBarrier& b = *barriers[k];
					bool start = contains(expandStages(b.src.stage), expandStages(seq[i].stage));
					if (writes(seq[i]))
						start = start && contains(expandAccess(b.src.access), expandAccess(seq[i].access & kSpecBufferWrites));
					bool chained = false;
					for (size_t m = i + 1; m < k; ++m)
						chained |= reached[m] && (expandStages(barriers[m]->dst.stage) & expandStages(b.src.stage)) != 0;
					if (!start && !chained)
						continue;
					reached[k] = true;
					ordered = contains(expandStages(b.dst.stage), expandStages(seq[j].stage)) && (!writes(seq[i]) || contains(expandAccess(b.dst.access), expandAccess(seq[j].access)));
				}
				if (!ordered)
					failures.push_back("access " + std::to_string(j) + " not ordered after access " + std::to_string(i));
			}
		}
		return failures;
	}

	namespace known_bad {
		// tracker before the fix: one StageAccess per buffer, overwritten by every request
		std::optional<vkutil::BufferBarrier> lastRequestOnly(vkutil::BufferAccessTracker& t, StageAccess a) {
			const StageAccess current = t.write;
			t.write = a;
			const bool anyWrite = (current.access | a.access) & kSpecBufferWrites;
			if (current.stage == 0 || !anyWrite)
				return std::nullopt;
			return vkutil::BufferBarrier{current, a};
		}
	} // namespace known_bad

	const StageAccess kFragmentRead = vkutil::GetBufferStageAccess(BufferAccess::ShaderRead, PassDesc::Type::Graphics);
	const StageAccess kComputeRead = vkutil::GetBufferStageAccess(BufferAccess::ShaderRead, PassDesc::Type::Compute);
	const StageAccess kComputeWrite = vkutil::GetBufferStageAccess(BufferAccess::ShaderWrite, PassDesc::Type::Compute);
} // namespace

TEST_CASE("render graph buffer tracker orders every hazard for every request sequence up to length 4") {
	const std::vector<StageAccess> requests = everyRequest();
	const size_t n = requests.size();
	size_t checked = 0;
	std::vector<StageAccess> seq;
	const auto visit = [&](auto&& self, size_t depth) -> void {
		if (!seq.empty()) {
			++checked;
			for (const std::string& failure : check(vkutil::TrackBufferAccess, seq))
				FAIL_CHECK("sequence of " << seq.size() << " starting with stage " << seq[0].stage << ": " << failure);
		}
		if (depth == 4)
			return;
		for (const StageAccess& a : requests) {
			seq.push_back(a);
			self(self, depth + 1);
			seq.pop_back();
		}
	};
	visit(visit, 0);
	CHECK(checked == n + n * n + n * n * n + n * n * n * n);
}

TEST_CASE("checker catches the buffer tracker bugs that actually happened") {
	SUBCASE("earlier reader dropped: fragment read, compute read, compute write") {
		CHECK_FALSE(check(known_bad::lastRequestOnly, {kFragmentRead, kComputeRead, kComputeWrite}).empty());
		CHECK(check(vkutil::TrackBufferAccess, {kFragmentRead, kComputeRead, kComputeWrite}).empty());
	}
	SUBCASE("later reader in another stage never synced with the write: compute write, fragment read, compute read") {
		CHECK_FALSE(check(known_bad::lastRequestOnly, {kComputeWrite, kFragmentRead, kComputeRead}).empty());
		CHECK(check(vkutil::TrackBufferAccess, {kComputeWrite, kFragmentRead, kComputeRead}).empty());
	}
}
