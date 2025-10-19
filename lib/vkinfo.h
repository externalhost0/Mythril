//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once
#include <span>
#include <volk.h>

namespace mythril::vkinfo {
	VkCommandPoolCreateInfo CreateCommandPoolInfo(uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0);
	VkCommandBufferAllocateInfo CreateCommandBufferAllocateInfo(VkCommandPool pool, uint32_t count);
	VkCommandBufferBeginInfo CreateCommandBufferBeginInfo(VkCommandBufferUsageFlags flags = 0);
	VkCommandBufferSubmitInfo CreateCommandBufferSubmitInfo(VkCommandBuffer cmd);

	VkFenceCreateInfo CreateFenceInfo(VkFenceCreateFlags flags = 0);
	VkSemaphoreCreateInfo CreateSemaphoreInfo(VkSemaphoreCreateFlags flags = 0);
	VkSemaphoreSubmitInfo CreateSemaphoreSubmitInfo(VkPipelineStageFlags2 stageMask, VkSemaphore semaphore);
	VkSubmitInfo2 CreateSubmitInfo(VkCommandBufferSubmitInfo* cmd, VkSemaphoreSubmitInfo* signalSemaphoreInfo, VkSemaphoreSubmitInfo* waitSemaphoreInfo);

	VkPipelineShaderStageCreateInfo CreatePipelineShaderStageInfo(VkShaderStageFlagBits stage, VkShaderModule shaderModule, const char* entry = "main");
	// two versions, same thing
	VkPipelineLayoutCreateInfo CreatePipelineLayoutInfo();
	VkPipelineLayoutCreateInfo CreatePipelineLayoutInfo(VkPushConstantRange* push_constant, VkDescriptorSetLayout* descriptor_layout);
	VkPipelineLayoutCreateInfo CreatePipelineLayoutInfo(VkPushConstantRange* push_constants, std::span<VkDescriptorSetLayout> descriptor_layout);
	VkPipelineLayoutCreateInfo CreatePipelineLayoutInfo(std::span<VkPushConstantRange> push_constants, VkDescriptorSetLayout* descriptor_layout);
	VkPipelineLayoutCreateInfo CreatePipelineLayoutInfo(std::span<VkPushConstantRange> push_constants, std::span<VkDescriptorSetLayout> descriptor_layouts);

	VkRenderingAttachmentInfo CreateColorAttachmentInfo(VkImageView view, const VkClearColorValue* clear, VkImageView resolveView = nullptr, VkResolveModeFlags resolveFlags = VK_RESOLVE_MODE_AVERAGE_BIT);
	VkRenderingAttachmentInfo CreateColorAttachmentInfo(VkImageView view, const VkClearColorValue* clear, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkImageView resolveView = nullptr, VkResolveModeFlags resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT);
	VkRenderingAttachmentInfo CreateDepthAttachmentInfo(VkImageView view, const VkClearDepthStencilValue* clear);
	VkRenderingAttachmentInfo CreateDepthStencilAttachmentInfo(VkImageView view, const VkClearDepthStencilValue* clear);
	VkRenderingAttachmentInfo CreateDepthStencilAttachmentInfo(VkImageView view, const VkClearDepthStencilValue* clear, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkImageView resolveView = nullptr, VkResolveModeFlags resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT);

	VkRenderingInfo CreateRenderingInfo(VkExtent2D extent2D, VkRenderingAttachmentInfo* attachment, VkRenderingAttachmentInfo* depthAttachment);
	VkRenderingInfo CreateRenderingInfo(VkExtent2D extent2D, std::span<VkRenderingAttachmentInfo> colorAttachments, VkRenderingAttachmentInfo* depthAttachment);

	VkPipelineDynamicStateCreateInfo CreatePipelineDynamicStateInfo(const VkDynamicState* states, uint32_t statescount, VkPipelineDynamicStateCreateFlags flags = 0);

	VkImageViewCreateInfo CreateImageViewInfo(VkFormat format, VkImage image, VkImageAspectFlags aspectFlags);
	VkImageCreateInfo CreateImageInfo(VkExtent3D extent, VkFormat format, VkImageUsageFlags usage_flags, uint32_t mipmap_levels = 1, VkSampleCountFlags sample_flag = VK_SAMPLE_COUNT_1_BIT);
	VkImageSubresourceRange CreateImageSubresourceRange(VkImageAspectFlags aspectMask);

	VkImageMemoryBarrier2 CreateImageMemoryBarrier2(VkImage image, VkFormat format, VkImageLayout currentLayout, VkImageLayout newLayout, bool isResolveAttachment = false);
}