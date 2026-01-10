//
// Created by Hayden Rivas on 10/6/25.
//

#include "vkutil.h"
#include "vkstring.h"
#include "HelperMacros.h"
#include "Logger.h"

#include <array>
#include <algorithm>

#include <volk.h>

namespace mythril::vkutil {
	enum class IOSurfacePixelFormat : uint32_t {
		Unknown     = 0,
		BGRA8888    = 'BGRA',  // 0x42475241
		RGBA8888    = 'RGBA',  // 0x52474241
		Luminance8  = 'L008',  // 0x4C303038
		NV12        = '420v',  // 0x34323076
		YUV420P     = 'y420',  // 0x79343230
	};

	struct TextureFormatProperties {
		const VkFormat format = VK_FORMAT_UNDEFINED;
		const uint8_t bytesPerBlock = 1;
		const uint8_t blockWidth = 1;
		const uint8_t blockHeight = 1;
		const uint8_t minBlocksX = 1;
		const uint8_t minBlocksY = 1;
		const bool depth = false;
		const bool stencil = false;
		const bool compressed = false;
		const uint8_t numPlanes = 1;
	};

#define PROPS(fmt, bpb, ...) TextureFormatProperties { VK_FORMAT_##fmt, bpb, ##__VA_ARGS__ }
	static constexpr std::array<TextureFormatProperties, 21> kTextureFormatTable = {{
		PROPS(R8_UNORM, 1),                       // 9
		PROPS(R8G8_UNORM, 2),                     // 16
		PROPS(R8G8B8A8_UNORM, 4),                 // 37
		PROPS(R8G8B8A8_SRGB, 4),                  // 43
		PROPS(B8G8R8A8_UNORM, 4),                 // 44
		PROPS(B8G8R8A8_SRGB, 4),                  // 50
		PROPS(R16_SFLOAT, 2),                     // 76
		PROPS(R16G16B16A16_SFLOAT, 8),            // 97
		PROPS(R32_UINT, 4),                       // 98
		PROPS(R32G32_UINT, 8),                    // 99
		PROPS(R32G32B32A32_UINT, 16),             // 100
		PROPS(R32G32B32A32_SFLOAT, 16),           // 109
		PROPS(D16_UNORM, 2, 1, 1, 1, 1, true),     // 124
		PROPS(D24_UNORM_S8_UINT, 4, 1, 1, 1, 1, true, true), // 129
		PROPS(D32_SFLOAT, 4, 1, 1, 1, 1, true),    // 126
		PROPS(D32_SFLOAT_S8_UINT, 5, 1, 1, 1, 1, true, true),// 130
		PROPS(BC7_UNORM_BLOCK, 16, 4, 4, 1, 1, false, false, true), // 145
		PROPS(ETC2_R8G8B8_UNORM_BLOCK, 8, 4, 4, 1, 1, false, false, true), // 147
		PROPS(ETC2_R8G8B8_SRGB_BLOCK, 8, 4, 4, 1, 1, false, false, true),  // 151
		PROPS(G8_B8R8_2PLANE_420_UNORM, 24, 4, 4, 1, 1, false, false, true, 2), // 1000157000
		PROPS(G8_B8_R8_3PLANE_420_UNORM, 24, 4, 4, 1, 1, false, false, true, 3), // 1000157001
	}};
//	static constexpr std::array<TextureFormatProperties, 32> kTextureFormatTable = {{
//		PROPS(UNDEFINED, 1),                      // 0
//		PROPS(R8_UNORM, 1),                       // 9
//		PROPS(R8G8_UNORM, 2),                     // 16
//		PROPS(R8G8B8A8_UNORM, 4),                 // 37
//		PROPS(R8G8B8A8_SRGB, 4),                  // 43
//		PROPS(B8G8R8A8_UNORM, 4),                 // 44
//		PROPS(B8G8R8A8_SRGB, 4),                  // 50
//		PROPS(A2R10G10B10_UNORM_PACK32, 4),       // 58
//		PROPS(A2B10G10R10_UNORM_PACK32, 4),       // 64
//		PROPS(R16_UNORM, 2),                      // 70
//		PROPS(R16_UINT, 2),                       // 74
//		PROPS(R16_SFLOAT, 2),                     // 76
//		PROPS(R16G16_UNORM, 4),                   // 77
//		PROPS(R16G16_UINT, 4),                    // 81
//		PROPS(R16G16_SFLOAT, 4),                  // 83
//		PROPS(R16G16B16A16_SFLOAT, 8),            // 97
//		PROPS(R32_UINT, 4),                       // 98
//		PROPS(R32_SFLOAT, 4),                     // 100
//		PROPS(R32G32_UINT, 8),                    // 101
//		PROPS(R32G32_SFLOAT, 8),                  // 103
//		PROPS(R32G32B32A32_UINT, 16),             // 107
//		PROPS(R32G32B32A32_SFLOAT, 16),           // 109
//		PROPS(D16_UNORM, 2, 1, 1, 1, 1, true),                         // 124
//		PROPS(D32_SFLOAT, 4, 1, 1, 1, 1, true),                       // 126
//		PROPS(D24_UNORM_S8_UINT, 4, 1, 1, 1, 1, true, true),          // 129
//		PROPS(D32_SFLOAT_S8_UINT, 5, 1, 1, 1, 1, true, true),         // 130
//		PROPS(BC7_UNORM_BLOCK, 16, 4, 4, 1, 1, false, false, true),   // 145
//		PROPS(ETC2_R8G8B8_UNORM_BLOCK, 8, 4, 4, 1, 1, false, false, true),  // 147
//		PROPS(ETC2_R8G8B8_SRGB_BLOCK, 8, 4, 4, 1, 1, false, false, true),   // 148
//		PROPS(G8_B8R8_2PLANE_420_UNORM, 24, 4, 4, 1, 1, false, false, true, 2),  // 1000156000
//		PROPS(G8_B8_R8_3PLANE_420_UNORM, 24, 4, 4, 1, 1, false, false, true, 3), // 1000156001
//	}};
#undef PROPS

	constexpr const TextureFormatProperties* GetFormatProperties(VkFormat format) {
		auto cmp = [](const TextureFormatProperties& a, VkFormat b) {
			return a.format < b;
		};
		auto it = std::lower_bound(kTextureFormatTable.begin(), kTextureFormatTable.end(), format, cmp);
		if (it != kTextureFormatTable.end() && it->format == format) {
			return &(*it);
		}
		ASSERT_MSG(false, "VkFormat '{}' not currently supported!", vkstring::VulkanFormatToString(format));
	}
	uint32_t GetTextureBytesPerLayer(uint32_t width, uint32_t height, VkFormat format, uint32_t level) {
		const uint32_t levelWidth  = std::max(width >> level, 1u);
		const uint32_t levelHeight = std::max(height >> level, 1u);

		const TextureFormatProperties* props = GetFormatProperties(format);
		ASSERT_MSG(props, "Unknown texture format!");

		if (!props->compressed) {
			return props->bytesPerBlock * levelWidth * levelHeight;
		}
		uint32_t blockWidth = std::max<uint32_t>(props->blockWidth, 1);
		uint32_t blockHeight = std::max<uint32_t>(props->blockHeight, 1);
		uint32_t widthInBlocks = (levelWidth + blockWidth - 1) / blockWidth;
		uint32_t heightInBlocks = (levelHeight + blockHeight - 1) / blockHeight;
		return widthInBlocks * heightInBlocks * props->bytesPerBlock;
	}
	uint32_t GetTextureBytesPerPlane(uint32_t width, uint32_t height, VkFormat format, uint32_t plane) {
		const TextureFormatProperties* props = GetFormatProperties(format);
		ASSERT(plane < props->numPlanes);
		switch (format) {
			case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
				return width * height / (plane + 1);
			case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
				return width * height / (plane ? 4 : 1);
			default:;
		}
		return GetTextureBytesPerLayer(width, height, format, 0);
	}
	VkExtent2D GetImagePlaneExtent(VkExtent2D plane0, VkFormat format, uint32_t plane) {
		switch (format) {
			case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
				return VkExtent2D{
						.width = plane0.width >> plane,
						.height = plane0.height >> plane,
				};
			case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
				return VkExtent2D{
						.width = plane0.width >> (plane ? 1 : 0),
						.height = plane0.height >> (plane ? 1 : 0),
				};
			default:;
		}
		return plane0;
	}

	VkImageAspectFlags AspectMaskFromAttachmentLayout(VkImageLayout layout) {
		switch (layout) {
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_STENCIL_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

			default: return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	VkImageAspectFlags AspectMaskFromFormat(VkFormat format) {
		switch (format) {
			case VK_FORMAT_D16_UNORM:
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_X8_D24_UNORM_PACK32:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	uint32_t GetBytesPerPixel(VkFormat format) {
		const TextureFormatProperties* props = GetFormatProperties(format);
		ASSERT_MSG(props, "Unknown VkFormat: {}", static_cast<int>(format));
		ASSERT_MSG(!props->compressed, "Compressed formats do not have meaningful bytes-per-pixel! Use GetTextureBytesPerLayer() for compressed formats.");

		return props->bytesPerBlock;
	}
	uint32_t GetNumImagePlanes(VkFormat format) {
		if (format == VK_FORMAT_UNDEFINED)
			return 0;

		const TextureFormatProperties* props = GetFormatProperties(format);
		ASSERT_MSG(props, "Unknown VkFormat: {}", static_cast<int>(format));
		return props->numPlanes;
	}
	uint32_t GetPixelFormat(VkFormat format) {
		const TextureFormatProperties* props = GetFormatProperties(format);
		ASSERT_MSG(props, "Unknown VkFormat: {}", static_cast<int>(format));
		return props->format;
	}

	// we make alot of calls to this so this small function helps
	void ImageMemoryBarrier2(VkCommandBuffer cmd, VkImage image, StageAccess src, StageAccess dst, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange range) {
		const VkImageMemoryBarrier2 barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.pNext = nullptr,

				.srcStageMask = src.stage,
				.srcAccessMask = src.access,
				.dstStageMask = dst.stage,
				.dstAccessMask = dst.access,

				.oldLayout = oldLayout,
				.newLayout = newLayout,

				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

				.image = image,
				.subresourceRange = range
		};
		const VkDependencyInfo di = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.pNext = nullptr,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrier
		};
		vkCmdPipelineBarrier2(cmd, &di);
	}
	StageAccess getPipelineStageAccess(VkImageLayout layout) {
		switch (layout) {
			case VK_IMAGE_LAYOUT_UNDEFINED:
				return {
						.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
						.access = VK_ACCESS_2_NONE,
				};
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
						.access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				};
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
						.access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				};
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
								 VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT,
						.access = VK_ACCESS_2_SHADER_READ_BIT,
				};
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
						.access = VK_ACCESS_2_TRANSFER_READ_BIT,
				};
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
						.access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
				};
			case VK_IMAGE_LAYOUT_GENERAL:
				return {
						.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
						.access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
				};
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				return {
						.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
						.access = VK_ACCESS_2_NONE | VK_ACCESS_2_SHADER_WRITE_BIT,
				};
			default:
				ASSERT_MSG(false, "Unsupported image layout transition!");
		}
	};
	VkSemaphore CreateTimelineSemaphore(VkDevice device, unsigned int numImages) {
		const VkSemaphoreTypeCreateInfo smeaphore_type_ci = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue = numImages,
		};
		const VkSemaphoreCreateInfo semaphore_ci = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &smeaphore_type_ci,
		};
		VkSemaphore semaphore;
		VK_CHECK(vkCreateSemaphore(device, &semaphore_ci, nullptr, &semaphore));
		return semaphore;
	}


}
