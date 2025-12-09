//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once
#include <cstdint>

namespace mythril::vkutil {
	struct StageAccess
	{
		VkPipelineStageFlags2 stage;
		VkAccessFlags2 access;
	};
	uint32_t GetTextureBytesPerPlane(uint32_t width, uint32_t height, VkFormat format, uint32_t plane);
	uint32_t GetTextureBytesPerLayer(uint32_t width, uint32_t height, VkFormat format, uint32_t level);
	uint32_t GetBytesPerPixel(VkFormat format);
	uint32_t GetNumImagePlanes(VkFormat format);
	VkExtent2D GetImagePlaneExtent(VkExtent2D plane0, VkFormat format, uint32_t plane);

	uint32_t GetAlignedSize(uint32_t value, uint32_t alignment);

	VkImageAspectFlags AspectMaskFromAttachmentLayout(VkImageLayout layout);
	VkImageAspectFlags AspectMaskFromFormat(VkFormat format);

	bool IsFormatDepth(VkFormat format);
	bool IsFormatStencil(VkFormat format);
	bool IsFormatDepthOrStencil(VkFormat format);
	bool IsFormatDepthAndStencil(VkFormat format);

	StageAccess getPipelineStageAccess(VkImageLayout layout);
	void ImageMemoryBarrier2(VkCommandBuffer cmd, VkImage image, StageAccess src, StageAccess dst, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange range);

	uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

	VkSemaphore CreateTimelineSemaphore(VkDevice device, unsigned int numImages);
}