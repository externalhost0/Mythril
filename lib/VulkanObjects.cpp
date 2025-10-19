//
// Created by Hayden Rivas on 10/8/25.
//
#include "VulkanObjects.h"
#include "HelperMacros.h"
#include "vkutil.h"
#include "Logger.h"
#include "CTX.h"

#include <cstring>
namespace mythril {
	void AllocatedBuffer::bufferSubData(const CTX& ctx, size_t offset, size_t size, const void* data) {
		// only host-visible buffers can be uploaded this way
		if (!_mappedPtr) {
			return;
		}
		ASSERT(offset + size <= _bufferSize);
		if (data) {
			memcpy((uint8_t*)_mappedPtr + offset, data, size);
		} else {
			memset((uint8_t*)_mappedPtr + offset, 0, size);
		}

		if (!_isCoherentMemory) {
			flushMappedMemory(ctx, offset, size);
		}
	}
	void AllocatedBuffer::getBufferSubData(const CTX& ctx, size_t offset, size_t size, void* data) {
		// only host-visible buffers can be downloaded this way
		if (!_mappedPtr) {
			return;
		}
		ASSERT(offset + size <= _bufferSize);
		if (!_isCoherentMemory) {
			invalidateMappedMemory(ctx, offset, size);
		}
		memcpy(data, (const uint8_t*)_mappedPtr + offset, size);
	}
	void AllocatedBuffer::flushMappedMemory(const CTX &ctx, VkDeviceSize offset, VkDeviceSize size) const {
		if (!isMapped()) {
			return;
		}
		vmaFlushAllocation(ctx._vmaAllocator, _vmaAllocation, offset, size);
	}
	void AllocatedBuffer::invalidateMappedMemory(const CTX &ctx, VkDeviceSize offset, VkDeviceSize size) const {
		if (!isMapped()) {
			return;
		}
		vmaInvalidateAllocation(ctx._vmaAllocator, _vmaAllocation, offset, size);
	}
	void AllocatedTexture::transitionLayout(VkCommandBuffer cmd, VkImageLayout newImageLayout, const VkImageSubresourceRange& subresourceRange) {
		const VkImageLayout oldImageLayout = (_vkCurrentImageLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL)
											 ? (isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
											 : _vkCurrentImageLayout;
		if (newImageLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL) {
			newImageLayout = isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		vkutil::StageAccess src = vkutil::getPipelineStageAccess(oldImageLayout);
		vkutil::StageAccess dst = vkutil::getPipelineStageAccess(newImageLayout);

		if (isDepthAttachment() && _isResolveAttachment) {
			// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#renderpass-resolve-operations
			src.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			dst.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			src.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			dst.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		}
		vkutil::ImageMemoryBarrier2(cmd, _vkImage, src, dst, oldImageLayout, newImageLayout, subresourceRange);
		_vkCurrentImageLayout = newImageLayout;
	}
	void AllocatedTexture::generateMipmap(VkCommandBuffer cmd) {
		// Check if device supports downscaling for color or depth/stencil buffer based on image format
		{
			const uint32_t formatFeatureMask = (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
			const bool hardwareDownscalingSupported = (_vkFormatProperties.optimalTilingFeatures & formatFeatureMask) == formatFeatureMask;

			// FIXME: the warning is printing a void *
			if (!hardwareDownscalingSupported) {
				LOG_USER(LogType::Warning, "Doesn't support hardware downscaling of this image format: {}", (void*)_vkFormat);
				return;
			}
		}
		const VkFilter blitFilter = [](bool isDepthOrStencilFormat, bool imageFilterLinear) {
			if (isDepthOrStencilFormat) {
				return VK_FILTER_NEAREST;
			}
			if (imageFilterLinear) {
				return VK_FILTER_LINEAR;
			}
			return VK_FILTER_NEAREST;
		}(vkutil::IsFormatDepthOrStencil(_vkFormat), _vkFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);

		const VkImageAspectFlags imageAspectFlags = vkutil::AspectMaskFromFormat(_vkFormat);
		const VkImageLayout originalImageLayout = _vkCurrentImageLayout;
		ASSERT(originalImageLayout != VK_IMAGE_LAYOUT_UNDEFINED);
		this->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VkImageSubresourceRange{imageAspectFlags, 0, 1, 0, _numLayers});

		// now make the mipmaps
		for (uint32_t layer = 0; layer < _numLayers; ++layer) {
			auto mipWidth = (int32_t)_vkExtent.width;
			auto mipHeight = (int32_t)_vkExtent.height;

			for (uint32_t i = 1; i < _numLevels; ++i) {
				// 1: Transition the i-th level to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; it will be copied into from the (i-1)-th layer
				vkutil::ImageMemoryBarrier2(cmd,
											_vkImage,
											vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .access = VK_ACCESS_2_NONE},
											vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
											VK_IMAGE_LAYOUT_UNDEFINED,
											VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											VkImageSubresourceRange{imageAspectFlags, i, 1, layer, 1});

				const int32_t nextLevelWidth = (mipWidth > 1) ? (mipWidth / 2) : 1;
				const int32_t nextLevelHeight = (mipHeight > 1) ? (mipHeight / 2) : 1;

				const VkOffset3D srcOffsets[2] = {
						VkOffset3D{0, 0, 0},
						VkOffset3D{mipWidth, mipHeight, 1},
				};
				const VkOffset3D dstOffsets[2] = {
						VkOffset3D{0, 0, 0},
						VkOffset3D{nextLevelWidth, nextLevelHeight, 1},
				};
				const VkImageBlit blit = {
						.srcSubresource = VkImageSubresourceLayers{imageAspectFlags, i - 1, layer, 1},
						.srcOffsets = {srcOffsets[0], srcOffsets[1]},
						.dstSubresource = VkImageSubresourceLayers{imageAspectFlags, i, layer, 1},
						.dstOffsets = {dstOffsets[0], dstOffsets[1]},
				};
				vkCmdBlitImage(cmd,
							   _vkImage,
							   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   _vkImage,
							   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   1,
							   &blit,
							   blitFilter);
				// 3: Transition i-th level to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL as it will be read from in the next iteration
				ImageMemoryBarrier2(cmd,
									_vkImage,
									vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
									vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT},
									VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
									VkImageSubresourceRange{imageAspectFlags, i, 1, layer, 1});

				mipWidth = nextLevelWidth;
				mipHeight = nextLevelHeight;
			}
		}
	}

}