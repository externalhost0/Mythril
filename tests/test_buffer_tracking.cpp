#include "RenderGraphInternal.h"
#include "vk_sync_tables.h"

#include <doctest/doctest.h>

#include <algorithm>
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

	// oracle, write access derived from the spec's access flag descriptions, not from vkutil::kWriteAccess
	constexpr VkAccessFlags2 kSpecBufferWrites = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	bool writes(StageAccess a) { return (expandAccess(a.access) & expandAccess(kSpecBufferWrites)) != 0; }
	bool banned(const vkutil::BufferBarrier& b) {
		return (b.src.stage | b.dst.stage) & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || (b.src.access | b.dst.access) & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
	}

	using TrackFn = std::optional<vkutil::BufferBarrier> (*)(vkutil::BufferAccessTracker&, StageAccess);

	std::vector<StageAccess> everyRequest() {
		constexpr BufferAccess kAccesses[] = {BufferAccess::ShaderRead, BufferAccess::ShaderWrite, BufferAccess::ShaderReadWrite, BufferAccess::IndexRead,
		                                      BufferAccess::IndirectRead, BufferAccess::TransferRead, BufferAccess::TransferWrite, BufferAccess::AccelerationStructureRead,
		                                      BufferAccess::AccelerationStructureBuildInput, BufferAccess::AccelerationStructureBuildRead, BufferAccess::AccelerationStructureBuildWrite,
		                                      BufferAccess::AccelerationStructureScratch};
		constexpr PassDesc::Type kPassTypes[] = {PassDesc::Type::Graphics, PassDesc::Type::Compute, PassDesc::Type::Transfer, PassDesc::Type::RayTracing};
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
			if (banned(b))
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

	using PlanFn = void (*)(std::vector<CompiledBufferBarrier>&, BufferHandle, StageAccess);

	// spec: barriers in one vkCmdPipelineBarrier2 do not chain, so each hazard with the earlier pass needs one barrier whose src covers it and whose dst covers the later use
	std::vector<std::string> checkPass(PlanFn plan, const StageAccess prior, const std::vector<StageAccess>& uses) {
		vkutil::BufferAccessTracker tracker = {};
		vkutil::TrackBufferAccess(tracker, prior);
		std::vector<CompiledBufferBarrier> planned;
		for (const StageAccess& use : uses)
			plan(planned, {}, use);
		std::vector<vkutil::BufferBarrier> batch;
		for (const CompiledBufferBarrier& request : planned)
			if (const std::optional<vkutil::BufferBarrier> barrier = vkutil::TrackBufferAccess(tracker, request.dstMask))
				batch.push_back(*barrier);

		std::vector<std::string> failures;
		uint64_t useStages = 0, useAccess = 0;
		for (const StageAccess& use : uses) {
			useStages |= expandStages(use.stage);
			useAccess |= expandAccess(use.access);
		}
		for (const vkutil::BufferBarrier& b : batch) {
			if (banned(b))
				failures.push_back("ALL_COMMANDS/MEMORY_READ/MEMORY_WRITE is banned");
			if (!contains(expandStages(prior.stage), expandStages(b.src.stage)) || !contains(expandAccess(prior.access & kSpecBufferWrites), expandAccess(b.src.access)))
				failures.push_back("src not justified by the earlier pass");
			if (!contains(useStages, expandStages(b.dst.stage)) || !contains(useAccess, expandAccess(b.dst.access)))
				failures.push_back("dst wider than the pass uses");
		}
		for (size_t j = 0; j < uses.size(); ++j) {
			if (!writes(prior) && !writes(uses[j]))
				continue;
			const bool ordered = std::ranges::any_of(batch, [&](const vkutil::BufferBarrier& b) {
				const bool start = contains(expandStages(b.src.stage), expandStages(prior.stage)) && (!writes(prior) || contains(expandAccess(b.src.access), expandAccess(prior.access & kSpecBufferWrites)));
				return start && contains(expandStages(b.dst.stage), expandStages(uses[j].stage)) && (!writes(prior) || contains(expandAccess(b.dst.access), expandAccess(uses[j].access)));
			});
			if (!ordered)
				failures.push_back("use " + std::to_string(j) + " not ordered after the earlier pass");
		}
		return failures;
	}

	namespace known_bad {
		void onePerDeclaration(std::vector<CompiledBufferBarrier>& barriers, const BufferHandle handle, const StageAccess dstMask) { barriers.push_back({handle, dstMask}); }

		// tracker before the fix: one StageAccess per buffer, overwritten by every request
		std::optional<vkutil::BufferBarrier> lastRequestOnly(vkutil::BufferAccessTracker& t, StageAccess a) {
			const StageAccess current = t.write;
			t.write = a;
			const bool anyWrite = (current.access | a.access) & kSpecBufferWrites;
			if (current.stage == 0 || !anyWrite)
				return std::nullopt;
			return vkutil::BufferBarrier{current, a};
		}

		std::optional<vkutil::BufferBarrier> unionVisibility(vkutil::BufferAccessTracker& t, StageAccess a) {
			if (a.access & vkutil::kWriteAccess) {
				const StageAccess src = t.readStages ? StageAccess{.stage = t.readStages, .access = 0} : t.write;
				t = {.write = {.stage = a.stage, .access = a.access & vkutil::kWriteAccess}};
				return src.stage ? std::optional(vkutil::BufferBarrier{.src = src, .dst = a}) : std::nullopt;
			}
			VkPipelineStageFlags2& stageUnion = t.visible[0];
			VkPipelineStageFlags2& accessUnion = t.visible[1];
			t.readStages |= a.stage;
			if (t.write.stage == 0 || ((a.stage & ~stageUnion) == 0 && (a.access & ~accessUnion) == 0))
				return std::nullopt;
			stageUnion |= a.stage;
			accessUnion |= a.access;
			return vkutil::BufferBarrier{.src = t.write, .dst = a};
		}
	} // namespace known_bad

	const StageAccess kFragmentRead = vkutil::GetBufferStageAccess(BufferAccess::ShaderRead, PassDesc::Type::Graphics);
	const StageAccess kComputeRead = vkutil::GetBufferStageAccess(BufferAccess::ShaderRead, PassDesc::Type::Compute);
	const StageAccess kComputeWrite = vkutil::GetBufferStageAccess(BufferAccess::ShaderWrite, PassDesc::Type::Compute);
	const StageAccess kTransferWrite = vkutil::GetBufferStageAccess(BufferAccess::TransferWrite, PassDesc::Type::Transfer);
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

TEST_CASE("tracker checker rejects a tracker that only remembers the last request") {
	SUBCASE("earlier reader dropped: fragment read, compute read, compute write") {
		CHECK_FALSE(check(known_bad::lastRequestOnly, {kFragmentRead, kComputeRead, kComputeWrite}).empty());
		CHECK(check(vkutil::TrackBufferAccess, {kFragmentRead, kComputeRead, kComputeWrite}).empty());
	}
	SUBCASE("later reader in another stage never synced with the write: compute write, fragment read, compute read") {
		CHECK_FALSE(check(known_bad::lastRequestOnly, {kComputeWrite, kFragmentRead, kComputeRead}).empty());
		CHECK(check(vkutil::TrackBufferAccess, {kComputeWrite, kFragmentRead, kComputeRead}).empty());
	}
}

TEST_CASE("a buffer declared up to 3 times in one pass is ordered after every earlier access") {
	constexpr BufferAccess kAccesses[] = {BufferAccess::ShaderRead, BufferAccess::ShaderWrite, BufferAccess::ShaderReadWrite, BufferAccess::IndexRead,
	                                      BufferAccess::IndirectRead, BufferAccess::TransferRead, BufferAccess::TransferWrite, BufferAccess::AccelerationStructureRead,
	                                      BufferAccess::AccelerationStructureBuildInput, BufferAccess::AccelerationStructureBuildRead, BufferAccess::AccelerationStructureBuildWrite,
	                                      BufferAccess::AccelerationStructureScratch};
	constexpr PassDesc::Type kPassTypes[] = {PassDesc::Type::Graphics, PassDesc::Type::Compute, PassDesc::Type::Transfer, PassDesc::Type::RayTracing};
	const std::vector<StageAccess> priors = everyRequest();
	size_t checked = 0;
	for (const StageAccess& prior : priors)
		for (const PassDesc::Type type : kPassTypes) {
			std::vector<StageAccess> uses;
			const auto visit = [&](auto&& self) -> void {
				if (!uses.empty()) {
					++checked;
					for (const std::string& failure : checkPass(AddBufferBarrier, prior, uses))
						FAIL_CHECK("earlier stage " << prior.stage << ", " << uses.size() << " declarations, first stage " << uses[0].stage << ": " << failure);
				}
				if (uses.size() == 3)
					return;
				for (const BufferAccess access : kAccesses) {
					uses.push_back(vkutil::GetBufferStageAccess(access, type));
					self(self);
					uses.pop_back();
				}
			};
			visit(visit);
		}
	constexpr size_t n = std::size(kAccesses);
	CHECK(checked == priors.size() * std::size(kPassTypes) * (n + n * n + n * n * n));
}

TEST_CASE("tracker checker rejects visibility kept as one stage union and one access union") {
	const StageAccess fragmentWrite = vkutil::GetBufferStageAccess(BufferAccess::ShaderWrite, PassDesc::Type::Graphics);
	const StageAccess computeStructureRead = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureRead, PassDesc::Type::Compute);
	const std::vector seq = {fragmentWrite, kFragmentRead, computeStructureRead, kComputeRead};
	CHECK_FALSE(check(known_bad::unionVisibility, seq).empty());
	CHECK(check(vkutil::TrackBufferAccess, seq).empty());
}

TEST_CASE("acceleration structure accesses match what the spec says a build and a trace touch") {
	// spec, vkCmdBuildAccelerationStructuresKHR: dst and scratch are read and written, geometry and instance data is a shader read, every structure it reads is an acceleration structure read
	constexpr VkPipelineStageFlags2 kBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	constexpr VkAccessFlags2 kReadWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	const struct {
		BufferAccess access;
		StageAccess spec;
	} kBuildUses[] = {
	    {BufferAccess::AccelerationStructureBuildWrite, {kBuild, kReadWrite}},
	    {BufferAccess::AccelerationStructureScratch, {kBuild, kReadWrite}},
	    {BufferAccess::AccelerationStructureBuildInput, {kBuild, VK_ACCESS_2_SHADER_READ_BIT}},
	    {BufferAccess::AccelerationStructureBuildRead, {kBuild, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR}},
	};
	// spec, ray tracing and ray query: a shader reads the structure with an acceleration structure read in its own stage
	const struct {
		PassDesc::Type type;
		VkPipelineStageFlags2 stages;
	} kTracers[] = {
	    {PassDesc::Type::Graphics, VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT},
	    {PassDesc::Type::Compute, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
	    {PassDesc::Type::RayTracing, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR},
	};
	size_t checked = 0;
	for (const auto& use : kBuildUses) {
		const StageAccess got = vkutil::GetBufferStageAccess(use.access, PassDesc::Type::Transfer);
		CHECK(expandStages(got.stage) == expandStages(use.spec.stage));
		CHECK(expandAccess(got.access) == expandAccess(use.spec.access));
		CHECK(isWrite(use.access) == ((use.spec.access & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) != 0));
		++checked;
	}
	for (const auto& tracer : kTracers) {
		const StageAccess got = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureRead, tracer.type);
		CHECK(expandStages(got.stage) == expandStages(tracer.stages));
		CHECK(expandAccess(got.access) == expandAccess(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR));
		CHECK_FALSE(isWrite(BufferAccess::AccelerationStructureRead));
		++checked;
	}
	CHECK(checked == std::size(kBuildUses) + std::size(kTracers));
}

TEST_CASE("a structure build orders after the last trace and before the next one") {
	const StageAccess trace = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureRead, PassDesc::Type::RayTracing);
	const StageAccess build = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureBuildWrite, PassDesc::Type::Transfer);
	CHECK(check(vkutil::TrackBufferAccess, {trace, build, trace, build}).empty());
	CHECK(isWrite(BufferAccess::AccelerationStructureBuildWrite));
	CHECK(isWrite(BufferAccess::AccelerationStructureScratch));
	CHECK_FALSE(isWrite(BufferAccess::AccelerationStructureBuildRead));
}

TEST_CASE("pass checker rejects one barrier per declaration: transfer write, then compute write and read in one pass") {
	CHECK_FALSE(checkPass(known_bad::onePerDeclaration, kTransferWrite, {kComputeWrite, kComputeRead}).empty());
	CHECK(checkPass(AddBufferBarrier, kTransferWrite, {kComputeWrite, kComputeRead}).empty());
}
