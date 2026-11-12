#include "vkutil.h"

#include <doctest/doctest.h>
#include <vulkan/vk_enum_string_helper.h>

using namespace mythril;

namespace {
	struct SampleTexture {
		const char* where;
		uint8_t usage;
		StorageType storage;
		VkFormat format;
		VkImageUsageFlags expected;
	};

	constexpr uint8_t kAttach = TextureUsageBits_Attachment;
	constexpr uint8_t kSampled = TextureUsageBits_Sampled;
	constexpr uint8_t kStorage = TextureUsageBits_Storage;
	constexpr VkImageUsageFlags kDst = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	constexpr VkImageUsageFlags kSrc = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	constexpr VkImageUsageFlags kColor = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	constexpr VkImageUsageFlags kDepth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	constexpr VkImageUsageFlags kTransient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	constexpr VkImageUsageFlags kSampledBit = VK_IMAGE_USAGE_SAMPLED_BIT;
	constexpr VkImageUsageFlags kStorageBit = VK_IMAGE_USAGE_STORAGE_BIT;

	constexpr SampleTexture kSampleTextures[] = {
		{"03/04/07 msaa color", kAttach, StorageType::Memoryless, VK_FORMAT_R16G16B16A16_SFLOAT, kColor | kTransient},
		{"04/07 msaa depth", kAttach, StorageType::Memoryless, VK_FORMAT_D32_SFLOAT_S8_UINT, kDepth | kTransient},
		{"02/03/05/06/07 color target", kAttach, StorageType::Device, VK_FORMAT_R8G8B8A8_UNORM, kDst | kColor | kSrc},
		{"02/03/06 depth target", kAttach, StorageType::Device, VK_FORMAT_D32_SFLOAT_S8_UINT, kDst | kDepth | kSrc},
		{"04/06 sampled color target", kAttach | kSampled, StorageType::Device, VK_FORMAT_R8G8B8A8_UNORM, kDst | kSampledBit | kColor | kSrc},
		{"07 shadow map", kAttach | kSampled, StorageType::Device, VK_FORMAT_D16_UNORM, kDst | kSampledBit | kDepth | kSrc},
		{"07 offscreen color", kAttach | kSampled | kStorage, StorageType::Device, VK_FORMAT_R16G16B16A16_SFLOAT, kDst | kSampledBit | kStorageBit | kColor | kSrc},
		{"07 material texture", kSampled, StorageType::Device, VK_FORMAT_R8G8B8A8_UNORM, kDst | kSampledBit | kSrc},
		{"07 luminance adaptation, bloom", kSampled | kStorage, StorageType::Device, VK_FORMAT_R16_SFLOAT, kDst | kSampledBit | kStorageBit | kSrc},
		{"07 current luminance, 08 traced shadow", kStorage, StorageType::Device, VK_FORMAT_R8_UNORM, kDst | kStorageBit | kSrc},
	};
}

TEST_CASE("ToVkImageUsage reproduces the flags createTexture gave every sample texture") {
	for (const SampleTexture& t : kSampleTextures) {
		const VkImageUsageFlags got = vkutil::ToVkImageUsage(t.usage, t.storage, t.format);
		CHECK_MESSAGE(got == t.expected, t.where << ": got " << string_VkImageUsageFlags(got) << ", expected " << string_VkImageUsageFlags(t.expected));
	}
	CHECK(std::size(kSampleTextures) == 10);
}

TEST_CASE("memoryless sample textures only carry attachment usage") {
	// VUID-VkImageCreateInfo-usage-00963, transient images may only add color, depth stencil or input attachment usage
	constexpr VkImageUsageFlags allowed = kTransient | kColor | kDepth | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	size_t checked = 0;
	for (const SampleTexture& t : kSampleTextures) {
		if (t.storage != StorageType::Memoryless)
			continue;
		++checked;
		const VkImageUsageFlags got = vkutil::ToVkImageUsage(t.usage, t.storage, t.format);
		CHECK_MESSAGE((got & ~allowed) == 0, t.where << ": " << string_VkImageUsageFlags(got & ~allowed));
	}
	CHECK(checked == 2);
}
