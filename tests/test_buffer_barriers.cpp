#include "sync_spec.h"

#include <doctest/doctest.h>
#include <vulkan/vk_enum_string_helper.h>

#include <string>
#include <vector>

using namespace mythril;
using namespace sync_spec;

namespace {
	uint64_t SupportedStages(const VkAccessFlags2 access) {
		for (const st::Entry& e : st::kAccessSupportedStages)
			if (e.bit == access)
				return e.mask;
		return 0;
	}

	constexpr vkutil::BufferSyncFeatures kFeatureSets[] = {{false, false}, {true, false}, {true, true}};

	std::string featureNames(vkutil::BufferSyncFeatures f) { return f.rayTracingPipeline ? "AS+RT" : f.accelerationStructure ? "AS" : "none"; }

	// oracle, derived from the spec's buffer usage descriptions, not from the vkutil table
	std::vector<StageAccess> possibleUses(VkBufferUsageFlags usage, vkutil::BufferSyncFeatures f) {
		uint64_t shaders = Stages(SupportedStages(VK_ACCESS_2_SHADER_STORAGE_READ_BIT));
		if (!f.rayTracingPipeline)
			shaders &= ~Stages(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
		const bool as = f.accelerationStructure;

		std::vector<StageAccess> uses;
		if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
			uses.push_back({shaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
			uses.push_back({shaders, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
		}
		if (as && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR});
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR});
		}
		if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT});
		if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) {
			uses.push_back({VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
			if (as)
				uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
		}
		if (as && (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
			uses.push_back({VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT});
		if (as && (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) {
			for (VkPipelineStageFlags2 stage : {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR}) {
				uses.push_back({stage, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR});
				uses.push_back({stage, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR});
			}
			uses.push_back({shaders, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR});
		}
		if (f.rayTracingPipeline && (usage & VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR))
			uses.push_back({VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR});
		if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
		if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			uses.push_back({VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
		return uses;
	}

	// vkCmdUpdateBuffer counts as a clear command in sync2, COPY is also required on purpose to stay conservative
	constexpr StageAccess kUpdateWrite = {VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
	constexpr StageAccess kUploadWrite = {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};

	std::string usageNames(VkBufferUsageFlags usage) {
		std::string out;
		for (VkBufferUsageFlags bit = 1; bit; bit <<= 1)
			if (usage & bit)
				out += (out.empty() ? "" : "|") + std::string(string_VkBufferUsageFlagBits(static_cast<VkBufferUsageFlagBits>(bit)));
		return out;
	}

	using BarrierFn = vkutil::UpdateBufferBarriers (*)(VkBufferUsageFlags, vkutil::BufferSyncFeatures);

	std::vector<std::string> check(BarrierFn fn, const StageAccess write, const VkBufferUsageFlags usage, const vkutil::BufferSyncFeatures f) {
		const vkutil::UpdateBufferBarriers b = fn(usage, f);
		const std::vector<StageAccess> uses = possibleUses(usage, f);
		const Step steps[] = {{"earlier use", {}, uses}, {"buffer write", {b.before}, {write}}, {"later use", {b.after}, uses}};
		return Violations(steps, f);
	}

	constexpr uint32_t kUsageBitCount = 6;
	constexpr StorageType kStorageTypes[] = {StorageType::Device, StorageType::HostVisible, StorageType::Memoryless};

} // namespace

static size_t checkEveryBuffer(BarrierFn fn, const StageAccess write, bool includeHostVisible) {
	size_t checked = 0;
	for (uint32_t bits = 0; bits < (1u << kUsageBitCount); ++bits) {
		for (StorageType storage : kStorageTypes) {
			const VkBufferUsageFlags usage = vkutil::ToVkBufferUsage(static_cast<BufferUsageBits>(bits), storage);
			if (!(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || (storage == StorageType::HostVisible && !includeHostVisible))
				continue;
			for (vkutil::BufferSyncFeatures f : kFeatureSets) {
				++checked;
				std::string all;
				for (const std::string& failure : check(fn, write, usage, f))
					all += "\n  " + failure;
				if (!all.empty())
					FAIL_CHECK("usage " << usageNames(usage) << " features=" << featureNames(f) << all);
			}
		}
	}
	return checked;
}

// TRANSFER_DST comes from Device storage or from the Storage / AccelerationStructureInputReadOnly bits
constexpr size_t kSubsets = 1u << kUsageBitCount;
constexpr size_t kSubsetsWithTransferDst = kSubsets - kSubsets / 4;

TEST_CASE("cmdUpdateBufferEXT barriers are valid, cover every access, and are tight for every buffer createBuffer can make") {
	CHECK(checkEveryBuffer(vkutil::GetUpdateBufferBarriers, kUpdateWrite, true) == std::size(kFeatureSets) * (kSubsets + 2 * kSubsetsWithTransferDst));
}

TEST_CASE("staging upload barriers are valid, cover every access, and are tight for every buffer createBuffer can make") {
	// host visible buffers are mapped and written from the host, Device and Memoryless go through staging
	CHECK(checkEveryBuffer(vkutil::GetUploadBufferBarriers, kUploadWrite, false) == std::size(kFeatureSets) * (kSubsets + kSubsetsWithTransferDst));
}

TEST_CASE("every usage bit createBuffer can emit is handled by the barrier table") {
	VkBufferUsageFlags emitted = 0;
	for (uint32_t bits = 0; bits < (1u << kUsageBitCount); ++bits)
		for (StorageType storage : kStorageTypes)
			emitted |= vkutil::ToVkBufferUsage(static_cast<BufferUsageBits>(bits), storage);
	CHECK_MESSAGE((emitted & ~vkutil::kHandledBufferUsage) == 0, "unhandled usage " << usageNames(emitted & ~vkutil::kHandledBufferUsage));
}
