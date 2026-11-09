#include "vkutil.h"
#include "vk_sync_tables.h"

#include <doctest/doctest.h>
#include <vulkan/vk_enum_string_helper.h>

#include <string>
#include <vector>

using namespace mythril;
namespace st = mythril::sync_tables;

namespace {
	struct Use {
		VkPipelineStageFlags2 stage;
		VkAccessFlags2 access;
		bool write;
	};

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

	template <size_t N>
	uint64_t known(const st::Entry (&table)[N]) {
		uint64_t out = 0;
		for (const st::Entry& e : table)
			out |= e.bit;
		return out;
	}

	template <size_t N>
	std::string names(const st::Entry (&table)[N], uint64_t mask) {
		std::string out;
		for (const st::Entry& e : table)
			if (mask & e.bit)
				out += (out.empty() ? "" : "|") + std::string(e.name);
		return out.empty() ? "NONE" : out;
	}
	std::string stageNames(VkPipelineStageFlags2 mask) { return names(st::kStageExpansion, mask); }
	std::string accessNames(VkAccessFlags2 mask) { return names(st::kAccessExpansion, mask); }

	template <size_t N>
	uint64_t supportedStages(const st::Entry (&table)[N], VkAccessFlags2 access) {
		for (const st::Entry& e : table)
			if (e.bit == access)
				return e.mask;
		return 0;
	}

	constexpr vkutil::BufferSyncFeatures kFeatureSets[] = {{false, false}, {true, false}, {true, true}};

	std::string featureNames(vkutil::BufferSyncFeatures f) { return f.rayTracingPipeline ? "AS+RT" : f.accelerationStructure ? "AS" : "none"; }

	// AGENTS: this is the oracle, write it from the spec's buffer usage descriptions, never from the vkutil table
	std::vector<Use> possibleUses(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
		uint64_t shaders = expandStages(supportedStages(st::kAccessSupportedStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT));
		if (!f.rayTracingPipeline)
			shaders &= ~expandStages(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
		const bool as = f.accelerationStructure;

		std::vector<Use> uses;
		if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
			uses.push_back({shaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false});
			uses.push_back({shaders, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true});
		}
		if (as && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, false});
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, true});
		}
		if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, false});
		if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) {
			uses.push_back({VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false});
			if (as)
				uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false});
		}
		if (as && (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT, false});
		if (as && (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) {
			for (VkPipelineStageFlags2 stage : {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR}) {
				uses.push_back({stage, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, false});
				uses.push_back({stage, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, true});
			}
			uses.push_back({shaders, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, false});
		}
		if (f.rayTracingPipeline && (usage & VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR))
			uses.push_back({VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR, false});
		if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false});
		if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true});
		return uses;
	}

	// vkCmdUpdateBuffer counts as a clear command in sync2, COPY is also required on purpose to stay conservative
	constexpr Use kUpdateWrite = {VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true};
	constexpr Use kUploadWrite = {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true};

	struct Required {
		uint64_t stages = 0;
		uint64_t access = 0;
	};

	struct Checker {
		VkBufferUsageFlags usage;
		vkutil::BufferSyncFeatures features;
		std::vector<std::string> failures;

		void fail(const std::string& what) { failures.push_back(what); }

		void valid(const char* label, StageAccess sa) {
			const std::string l(label);
			if (!sa.stage)
				fail(l + ": empty stage mask");
			if (const uint64_t unknown = sa.stage & ~st::kAllStages)
				fail(l + ": unknown stage bits " + std::to_string(unknown));
			if (const uint64_t unknown = sa.access & ~(known(st::kAccessExpansion) & known(st::kAccessSupportedStages)))
				fail(l + ": unknown access bits " + std::to_string(unknown));
			if (sa.stage & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
				fail(l + ": ALL_COMMANDS is banned");
			if (sa.access & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT))
				fail(l + ": MEMORY_READ/MEMORY_WRITE is banned");
			if (!features.rayTracingPipeline && (sa.stage & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR))
				fail(l + ": ray tracing stage without the extension");
			if (!features.accelerationStructure && (sa.stage & (VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR)))
				fail(l + ": acceleration structure stage without the extension");
			const uint64_t stages = expandStages(sa.stage);
			for (const st::Entry& e : st::kAccessSupportedStages)
				if ((sa.access & e.bit) && !(e.mask & stages))
					fail(l + ": " + e.name + " not supported by " + stageNames(sa.stage));
		}
		void covers(const char* label, StageAccess sa, const Use& use, bool access) {
			if (const uint64_t missing = expandStages(use.stage) & ~expandStages(sa.stage))
				fail(std::string(label) + ": missing stages " + stageNames(missing) + " for " + accessNames(use.access));
			if (!access)
				return;
			if (const uint64_t missing = expandAccess(use.access) & ~expandAccess(sa.access))
				fail(std::string(label) + ": missing access " + accessNames(missing) + " at " + stageNames(use.stage));
		}
		void justified(const char* label, StageAccess sa, Required req) {
			for (const st::Entry& e : st::kStageExpansion)
				if ((sa.stage & e.bit) && !(e.mask & req.stages))
					fail(std::string(label) + ": unjustified stage " + e.name);
			for (const st::Entry& e : st::kAccessExpansion)
				if ((sa.access & e.bit) && !(e.mask & req.access))
					fail(std::string(label) + ": unjustified access " + e.name);
		}

		void writeThenConsumers(const char* src, const char* dst, const vkutil::BufferBarrier& b, const Use& write) {
			valid(src, b.src);
			valid(dst, b.dst);
			Required later;
			for (const Use& use : possibleUses(usage, features)) {
				covers(dst, b.dst, use, true);
				later.stages |= expandStages(use.stage);
				later.access |= expandAccess(use.access);
			}
			covers(src, b.src, write, true);
			justified(src, b.src, {expandStages(write.stage), expandAccess(write.access)});
			justified(dst, b.dst, later);
		}

		void run(const vkutil::UpdateBufferBarriers& b) {
			valid("before.src", b.before.src);
			valid("before.dst", b.before.dst);

			Required prior;
			for (const Use& use : possibleUses(usage, features)) {
				// earlier reads only need execution order, earlier writes also need to be made available
				covers("before.src", b.before.src, use, use.write);
				prior.stages |= expandStages(use.stage);
				if (use.write)
					prior.access |= expandAccess(use.access);
			}
			covers("before.dst", b.before.dst, kUpdateWrite, true);
			justified("before.src", b.before.src, prior);
			justified("before.dst", b.before.dst, {expandStages(kUpdateWrite.stage), expandAccess(kUpdateWrite.access)});
			writeThenConsumers("after.src", "after.dst", b.after, kUpdateWrite);
		}
	};

	std::string usageNames(VkBufferUsageFlags usage) {
		std::string out;
		for (VkBufferUsageFlags bit = 1; bit; bit <<= 1)
			if (usage & bit)
				out += (out.empty() ? "" : "|") + std::string(string_VkBufferUsageFlagBits(static_cast<VkBufferUsageFlagBits>(bit)));
		return out;
	}

	using BarrierFn = vkutil::UpdateBufferBarriers (*)(VkBufferUsageFlags, vkutil::BufferSyncFeatures);
	using UploadFn = vkutil::BufferBarrier (*)(VkBufferUsageFlags, vkutil::BufferSyncFeatures);

	std::vector<std::string> check(BarrierFn fn, VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
		Checker c{usage, f, {}};
		c.run(fn(usage, f));
		return c.failures;
	}
	std::vector<std::string> check(UploadFn fn, VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
		Checker c{usage, f, {}};
		c.writeThenConsumers("upload.src", "upload.dst", fn(usage, f), kUploadWrite);
		return c.failures;
	}

	template <typename Fn>
	bool anyFailureContains(Fn fn, VkBufferUsageFlags usage, const std::string& needle) {
		for (vkutil::BufferSyncFeatures f : kFeatureSets)
			for (const std::string& failure : check(fn, usage, f))
				if (failure.find(needle) != std::string::npos)
					return true;
		return false;
	}

	constexpr uint32_t kUsageBitCount = 6;
	constexpr StorageType kStorageTypes[] = {StorageType::Device, StorageType::HostVisible, StorageType::Memoryless};

	namespace known_bad {
		constexpr VkPipelineStageFlags2 guessedConsumerStages(VkBufferUsageFlags usage, bool rt) {
			VkPipelineStageFlags2 stages = 0;
			if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
				stages |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				if (rt)
					stages |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
			}
			if (usage & (VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
				stages |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
			if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
				stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			if (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)
				stages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
			return stages ? stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		}
		constexpr VkAccessFlags2 guessedConsumerAccess(VkBufferUsageFlags usage) {
			VkAccessFlags2 access = 0;
			if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
				access |= VK_ACCESS_2_UNIFORM_READ_BIT;
			if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
				access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
				access |= VK_ACCESS_2_INDEX_READ_BIT;
			if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
				access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
			if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
				access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			if (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)
				access |= VK_ACCESS_2_MEMORY_READ_BIT;
			return access ? access : VK_ACCESS_2_MEMORY_READ_BIT;
		}

		StageAccess guessedAccess(VkBufferUsageFlags usage, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, bool srcSide) {
			const VkPipelineStageFlags2 stage = srcSide ? src : dst;
			VkAccessFlags2 access = (stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) ? VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			if (!srcSide && (dst & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT))
				access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			if (!srcSide && (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
				access |= VK_ACCESS_2_INDEX_READ_BIT;
			return {stage, access};
		}
		vkutil::UpdateBufferBarriers allCommandsGuessedAccess(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
			const VkPipelineStageFlags2 consumers = guessedConsumerStages(usage, f.rayTracingPipeline);
			const auto pair = [&](VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst) {
				return vkutil::BufferBarrier{guessedAccess(usage, src, dst, true), guessedAccess(usage, src, dst, false)};
			};
			return {pair(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT), pair(VK_PIPELINE_STAGE_2_TRANSFER_BIT, consumers)};
		}

		vkutil::UpdateBufferBarriers noBdaConsumers(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
			const VkPipelineStageFlags2 consumers = guessedConsumerStages(usage, f.rayTracingPipeline);
			const VkAccessFlags2 priorWrites = VK_ACCESS_2_TRANSFER_WRITE_BIT | ((usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_NONE);
			const StageAccess transferWrite = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
			return {{{consumers | VK_PIPELINE_STAGE_2_TRANSFER_BIT, priorWrites}, transferWrite}, {transferWrite, {consumers, guessedConsumerAccess(usage)}}};
		}

		vkutil::UpdateBufferBarriers widened(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
			vkutil::UpdateBufferBarriers b = vkutil::GetUpdateBufferBarriers(usage, f);
			b.after.dst.stage |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
			b.after.dst.access |= VK_ACCESS_2_MEMORY_READ_BIT;
			return b;
		}

		vkutil::BufferBarrier sync1Staging(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures) {
			StageAccess dst = {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE};
			if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
				dst = {dst.stage | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, dst.access | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
			if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
				dst = {dst.stage | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, dst.access | VK_ACCESS_2_INDEX_READ_BIT};
			if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
				dst.access |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			if (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)
				dst = {dst.stage | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dst.access | VK_ACCESS_2_MEMORY_READ_BIT};
			return {{VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, dst};
		}
	} // namespace known_bad
} // namespace

template <typename Fn>
static size_t checkEveryBuffer(Fn fn, bool includeHostVisible) {
	size_t checked = 0;
	for (uint32_t bits = 0; bits < (1u << kUsageBitCount); ++bits) {
		for (StorageType storage : kStorageTypes) {
			const VkBufferUsageFlags usage = vkutil::ToVkBufferUsage(static_cast<uint8_t>(bits), storage);
			if (!(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || (storage == StorageType::HostVisible && !includeHostVisible))
				continue;
			for (vkutil::BufferSyncFeatures f : kFeatureSets) {
				++checked;
				const std::vector<std::string> failures = check(fn, usage, f);
				if (failures.empty())
					continue;
				std::string all;
				for (const std::string& failure : failures)
					all += "\n  " + failure;
				FAIL_CHECK("usage " << usageNames(usage) << " features=" << featureNames(f) << all);
			}
		}
	}
	return checked;
}

// TRANSFER_DST comes from Device storage or from the Storage / AccelerationStructureInputReadOnly bits
constexpr size_t kSubsets = 1u << kUsageBitCount;
constexpr size_t kSubsetsWithTransferDst = kSubsets - kSubsets / 4;

TEST_CASE("cmdUpdateBuffer barriers are valid, cover every access, and are tight for every buffer createBuffer can make") {
	CHECK(checkEveryBuffer(vkutil::GetUpdateBufferBarriers, true) == std::size(kFeatureSets) * (kSubsets + 2 * kSubsetsWithTransferDst));
}

TEST_CASE("staging upload barriers are valid, cover every access, and are tight for every buffer createBuffer can make") {
	// host visible buffers are mapped and written from the host, Device and Memoryless go through staging
	CHECK(checkEveryBuffer(vkutil::GetUploadBufferBarrier, false) == std::size(kFeatureSets) * (kSubsets + kSubsetsWithTransferDst));
}

TEST_CASE("every usage bit createBuffer can emit is handled by the barrier table") {
	VkBufferUsageFlags emitted = 0;
	for (uint32_t bits = 0; bits < (1u << kUsageBitCount); ++bits)
		for (StorageType storage : kStorageTypes)
			emitted |= vkutil::ToVkBufferUsage(static_cast<uint8_t>(bits), storage);
	CHECK_MESSAGE((emitted & ~vkutil::kHandledBufferUsage) == 0, "unhandled usage " << usageNames(emitted & ~vkutil::kHandledBufferUsage));
}

TEST_CASE("checker catches the barrier bugs that actually happened") {
	constexpr VkBufferUsageFlags device = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	SUBCASE("all commands with guessed access: INDEX_READ on a transfer stage") {
		CHECK(anyFailureContains(known_bad::allCommandsGuessedAccess, device | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "VK_ACCESS_2_INDEX_READ_BIT not supported"));
	}
	SUBCASE("all commands with guessed access: copy after update not ordered") {
		CHECK(anyFailureContains(known_bad::allCommandsGuessedAccess, device | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "after.dst: missing stages VK_PIPELINE_STAGE_2_COPY_BIT"));
	}
	SUBCASE("no BDA consumers: BDA shader write before update not waited on") {
		CHECK(anyFailureContains(known_bad::noBdaConsumers, device | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "before.src: missing stages VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT"));
		CHECK(anyFailureContains(known_bad::noBdaConsumers, device | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "before.src: missing access VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT"));
	}
	SUBCASE("no BDA consumers: copy after update not ordered") {
		CHECK(anyFailureContains(known_bad::noBdaConsumers, device | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "after.dst: missing stages VK_PIPELINE_STAGE_2_COPY_BIT"));
	}
	SUBCASE("widened: extra stage and MEMORY_READ on every buffer") {
		CHECK(anyFailureContains(known_bad::widened, device | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "after.dst: unjustified stage VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR"));
		CHECK(anyFailureContains(known_bad::widened, device | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "after.dst: MEMORY_READ/MEMORY_WRITE is banned"));
	}
	SUBCASE("sync1 staging: BDA reads of an indirect buffer not made visible") {
		CHECK(anyFailureContains(known_bad::sync1Staging, device | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "upload.dst: missing access VK_ACCESS_2_SHADER_STORAGE_READ_BIT"));
	}
	SUBCASE("sync1 staging: later copies not made visible") {
		CHECK(anyFailureContains(known_bad::sync1Staging, device | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "upload.dst: missing access VK_ACCESS_2_TRANSFER_READ_BIT"));
	}
}
