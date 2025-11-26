//
// Created by Hayden Rivas on 10/7/25.
//


#include "CommandBuffer.h"
#include "vkinfo.h"
#include "CTX.h"
#include "vkutil.h"
#include "Logger.h"
#include "GraphicsPipeline.h"

#include <volk.h>
#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#endif

namespace mythril {
	constexpr uint16_t kMaxColorAttachments = 16;

	// enforce that a valid CommandBuffer can only be created via this constructor
	CommandBuffer::CommandBuffer(CTX* ctx, CommandBuffer::Type type) : _ctx(ctx), _cmdType(type), _wrapper(&ctx->_imm->acquire()), _isDryRun(false) {};
	CommandBuffer::~CommandBuffer() {
		ASSERT_MSG(!_isRendering, "Please call to end rendering before destroying a Command Buffer!");
	}
	VkCommandBufferSubmitInfo CommandBuffer::requestSubmitInfo() const {
		VkCommandBufferSubmitInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		info.pNext = nullptr;
		info.commandBuffer = _wrapper->_cmdBuf;
		info.deviceMask = 0;
		return info;
	}

	// ALL PUBLIC COMMANDS AVAILBLE TO USER //
	void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
		if (_isDryRun) return;

		vkCmdDraw(_wrapper->_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
	}
	void CommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t baseInstance) {
		if (_isDryRun) return;

		vkCmdDrawIndexed(_wrapper->_cmdBuf, indexCount, instanceCount, firstIndex, vertexOffset, baseInstance);
	}

	void CommandBuffer::cmdBindIndexBuffer(InternalBufferHandle handle) {
		if (_isDryRun) return;

		const AllocatedBuffer& buffer = _ctx->viewBuffer(handle);
		vkCmdBindIndexBuffer(_wrapper->_cmdBuf, buffer._vkBuffer, 0, VK_INDEX_TYPE_UINT32);
	}
	void CommandBuffer::cmdTransitionLayout(InternalTextureHandle source, VkImageLayout newLayout) {
		if (_isDryRun) return;

		cmdTransitionLayoutImpl(source, _ctx->viewTexture(source)._vkCurrentImageLayout, newLayout);
	}
	void CommandBuffer::cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination) {
		if (_isDryRun) return;

		auto& sourceTex = _ctx->viewTexture(source);
		auto& destinationTex = _ctx->viewTexture(destination);
		if (sourceTex._vkCurrentImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			LOG_SYSTEM(LogType::Warning, "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) before blitting.", sourceTex._debugName);
			this->cmdTransitionLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		if (destinationTex._vkCurrentImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			LOG_SYSTEM(LogType::Warning, "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) before blitting.", destinationTex._debugName);
			this->cmdTransitionLayout(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}
		cmdBlitImageImpl(source, destination, sourceTex.getExtentAs2D(), destinationTex.getExtentAs2D());
	}
	void CommandBuffer::cmdCopyImage(InternalTextureHandle source, InternalTextureHandle destination) {
		if (_isDryRun) return;

		// needs to be same so doesnt matter if we take source or destination size
		cmdCopyImageImpl(source, destination, _ctx->viewTexture(source).getExtentAs2D());
	}

	void CommandBuffer::cmdPushConstants(const void* data, uint32_t size, uint32_t offset) {
		if (_isDryRun) return;

		ASSERT_MSG(size % 4 == 0, "Push constant size needs to be a multiple of 4. Is size {}", size);
		const VkPhysicalDeviceLimits& limits = _ctx->_vkPhysDeviceProperties.limits;
		if (size + offset > limits.maxPushConstantsSize) {
			LOG_SYSTEM(LogType::Error, "Push constants size exceeded %u (max %u bytes)", size + offset, limits.maxPushConstantsSize);
		}

		if (_currentPipelineHandle.empty()) {
			LOG_SYSTEM(LogType::Warning, "No pipeline currently bound, will not perform push constants!");
			return;
		}
		size_t pcsize = _ctx->_graphicsPipelinePool.get(_currentPipelineHandle)->signature.pushes[0].size;
		if (pcsize != size) {
//			LOG_SYSTEM(LogType::Warning, "Push constant CPU size is different than GPU size! Your CPU structure has a size of '{}' while shader's push constant has size of '{}'", size, pcsize);
		}

		const GraphicsPipeline& pipeline = *_ctx->_graphicsPipelinePool.get(_currentPipelineHandle);
		const VkShaderStageFlags stageFlags =  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		vkCmdPushConstants(_wrapper->_cmdBuf, pipeline._vkPipelineLayout, static_cast<VkShaderStageFlagBits>(stageFlags), offset, pcsize, data);
	}

	void MaybeWarnRebind(const IPipeline* pipeline, VkPipeline lastBound) {
		static std::unordered_map<VkPipeline, int> g_rebindWarningCount;
		VkPipeline vkPipe = pipeline->_vkPipeline;
		if (lastBound != vkPipe)
			return;
		int& count = g_rebindWarningCount[vkPipe];
		constexpr unsigned int maxWarns = 5;
		if (count < maxWarns) {
			LOG_SYSTEM(LogType::Warning, "Called to rebind already bound pipeline '{}'", pipeline->_debugName);
		} else if (count == maxWarns) {
			LOG_SYSTEM(LogType::Warning, "Pipeline '{}' has triggered this warning >{} times and will now be silenced.", pipeline->_debugName, maxWarns);
		}
		count++;
	}

	void CommandBuffer::cmdBindRenderPipeline(InternalGraphicsPipelineHandle handle) {
		ASSERT_MSG(_ctx != nullptr, "You forgot to assign ctx to your CommandBuffer dude.");
		if (_isDryRun) {
			// we perform construction inside our dry run, which is when we compile the RenderGraph
			// we do this so we dont stutter mid gameplay loop
			_ctx->resolveRenderPipeline(handle);
			return;
		}
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded render pipeline was empty/invalid!");
			return;
		}
		// use _currentPipeline
		_currentPipelineHandle = handle;
		const GraphicsPipeline* pipeline = _ctx->_graphicsPipelinePool.get(_currentPipelineHandle);

		// only bind pipeline if its not currently bound
		if (_lastBoundvkPipeline == pipeline->_vkPipeline) {
#ifdef DEBUG
			MaybeWarnRebind(static_cast<const IPipeline*>(pipeline), _lastBoundvkPipeline);
#endif
			return;
		}
		_lastBoundvkPipeline = pipeline->_vkPipeline;
		vkCmdBindPipeline(_wrapper->_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->_vkPipeline);

		// constructs another vector of descriptor sets but with the special bindless set in the correct index
		std::vector<VkDescriptorSet> vk_descriptor_sets = {};
		for (int i = 0; i < pipeline->signature.sets.size(); i++) {
			if (pipeline->signature.sets[i].isBindless) {
				vk_descriptor_sets.push_back(_ctx->_vkBindlessDSet);
				continue;
			}
			vk_descriptor_sets.push_back(pipeline->_dSets[i]);
		}
		vkCmdBindDescriptorSets(_wrapper->_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->_vkPipelineLayout,
								0,
								vk_descriptor_sets.size(),
								vk_descriptor_sets.data(),
								0,
								nullptr);
	}

	// current issues with host buffer
	void CommandBuffer::cmdUpdateBuffer(InternalBufferHandle handle, size_t offset, size_t size, const void* data) {
		if (_isDryRun) return;

		ASSERT_MSG(!_isRendering, "You cannot update a buffer while rendering! Please move this command either before or after rendering.");
		ASSERT(handle.valid());
		ASSERT(size && size <= 65536);
		ASSERT(size % 4 == 0);
		ASSERT(offset % 4 == 0);
		AllocatedBuffer* buf = _ctx->_bufferPool.get(handle);
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "You are updating buffer '{}' with empty data.", buf->_debugName);
			return;
		}

		bufferBarrierImpl(handle, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);

		vkCmdUpdateBuffer(_wrapper->_cmdBuf, buf->_vkBuffer, offset, size, data);

		VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		if (buf->_vkUsageFlags & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) {
			dstStage |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		}
		bufferBarrierImpl(handle, VK_PIPELINE_STAGE_2_TRANSFER_BIT, dstStage);
	}
	void CommandBuffer::cmdCopyImageToBuffer(InternalTextureHandle source, InternalBufferHandle destination, const VkBufferImageCopy& region) {
		if (_isDryRun) return;

		auto& sourceTex = _ctx->viewTexture(source);
		VkImageLayout currentLayout = sourceTex._vkCurrentImageLayout;
		if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && currentLayout != VK_IMAGE_LAYOUT_GENERAL) {
			cmdTransitionLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		VkImageLayout properLayout = sourceTex._vkCurrentImageLayout;
		vkCmdCopyImageToBuffer(_wrapper->_cmdBuf, sourceTex._vkImage, properLayout, _ctx->viewBuffer(destination)._vkBuffer, 1, &region);
	}

#ifdef MYTH_ENABLED_IMGUI
	void CommandBuffer::cmdDrawImGui() {
		if (_isDryRun) return;
		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _wrapper->_cmdBuf);
	}
#endif

// ALL INTERNALLY CALLED COMMAND BUFFER FUNCTIONS
	void CommandBuffer::cmdBeginRenderingImpl() {
		ASSERT_MSG(!_isRendering, "Command Buffer is already rendering!");

		// Step 1: Get all colorAttachmentInfos and a depthAttachmentInfo
		std::vector<VkRenderingAttachmentInfo> colorAttachmentsInfo = {};
		colorAttachmentsInfo.reserve(kMaxColorAttachments);
		for (const ColorAttachmentInfo& attachmentInfo : _activePass.colorAttachments) {
			const bool isClearing = attachmentInfo.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
			// TODO: clear color value should use the right type in its union depending on the texture's format
			// for now its just using floats
			if (attachmentInfo.resolveImageView != VK_NULL_HANDLE) {
				colorAttachmentsInfo.push_back(vkinfo::CreateColorAttachmentInfo(attachmentInfo.imageView,
																				 isClearing ? &attachmentInfo.clearColor : nullptr,
																				 attachmentInfo.loadOp,
																				 attachmentInfo.storeOp,
																				 attachmentInfo.resolveImageView,
																				 toVulkan(ResolveMode::AVERAGE)));
			} else {
				colorAttachmentsInfo.push_back(vkinfo::CreateColorAttachmentInfo(attachmentInfo.imageView,
																				 isClearing ? &attachmentInfo.clearColor : nullptr,
																				 attachmentInfo.loadOp,
																				 attachmentInfo.storeOp));
			}
		}
		const bool hasDepth = _activePass.depthAttachment.has_value();
		VkRenderingAttachmentInfo depthAttachmentInfo = {};
		if (hasDepth) {
			const DepthAttachmentInfo& attachmentInfo = _activePass.depthAttachment.value();
			const bool isClearing = attachmentInfo.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachmentInfo = vkinfo::CreateDepthStencilAttachmentInfo(attachmentInfo.imageView,
																		   isClearing ? &attachmentInfo.clearDepthStencil : nullptr,
																		   attachmentInfo.loadOp,
																		   attachmentInfo.storeOp);
		}
		// Step 2: Put it together in a renderingInfo struct
		VkExtent2D renderExtent = _activePass.extent2D;
		VkRenderingInfo info = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
				.pNext = nullptr,
				.flags = 0,
				.renderArea = {
						.offset = { 0, 0 },
						.extent = renderExtent
				},
				.layerCount = 1,
				.viewMask = 0,
				.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentsInfo.size()),
				.pColorAttachments = colorAttachmentsInfo.data(),
				.pDepthAttachment = hasDepth ? &depthAttachmentInfo : nullptr,
				.pStencilAttachment = nullptr
		};
		cmdSetScissorImpl(renderExtent);
		cmdSetViewportImpl(renderExtent);
		cmdBindDepthStateImpl({});

		_ctx->checkAndUpdateBindlessDescriptorSetImpl();
//		_ctx->checkAndUpdateDescriptorSets();

		vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, VK_COMPARE_OP_ALWAYS);
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, VK_FALSE);
		vkCmdBeginRendering(_wrapper->_cmdBuf, &info);
		_isRendering = true;
	}

	void CommandBuffer::cmdEndRenderingImpl() {
		vkCmdEndRendering(_wrapper->_cmdBuf);
		_isRendering = false;
	}

	void CommandBuffer::cmdPrepareToSwapchainImpl(InternalTextureHandle source) {
		cmdTransitionLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		cmdTransitionSwapchainLayoutImpl(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		cmdBlitToSwapchainImpl(source);
		cmdTransitionSwapchainLayoutImpl(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	void CommandBuffer::bufferBarrierImpl(InternalBufferHandle bufhandle, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
		auto& buf = _ctx->viewBuffer(bufhandle);

		VkBufferMemoryBarrier2 barrier = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = srcStage,
				.srcAccessMask = 0,
				.dstStageMask = dstStage,
				.dstAccessMask = 0,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buf._vkBuffer,
				.offset = 0,
				.size = VK_WHOLE_SIZE,
		};
		if (srcStage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
			barrier.srcAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
		} else {
			barrier.srcAccessMask |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
		}
		if (dstStage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
			barrier.dstAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
		} else {
			barrier.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
		}
		if (dstStage & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) {
			barrier.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		}
		if (buf._vkUsageFlags & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
			barrier.dstAccessMask |= VK_ACCESS_2_INDEX_READ_BIT;
		}
		const VkDependencyInfo depInfo = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = 1,
				.pBufferMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(_wrapper->_cmdBuf, &depInfo);
	}
	// TODO: have this not repeat the logic already specified in cmdBlitImage, all you need is swapchains current texturehandle
	void CommandBuffer::cmdBlitToSwapchainImpl(InternalTextureHandle source) {
		VkImageBlit2 blitRegion = { .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

		blitRegion.srcOffsets[1].x = static_cast<int32_t>(_ctx->viewTexture(source)._vkExtent.width);
		blitRegion.srcOffsets[1].y = static_cast<int32_t>(_ctx->viewTexture(source)._vkExtent.height);
		blitRegion.srcOffsets[1].z = 1;

		blitRegion.dstOffsets[1].x = static_cast<int32_t>(_ctx->_swapchain->getSwapchainExtent().width);
		blitRegion.dstOffsets[1].y = static_cast<int32_t>(_ctx->_swapchain->getSwapchainExtent().height);
		blitRegion.dstOffsets[1].z = 1;

		blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.srcSubresource.baseArrayLayer = 0;
		blitRegion.srcSubresource.layerCount = 1;
		blitRegion.srcSubresource.mipLevel = 0;

		blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.dstSubresource.baseArrayLayer = 0;
		blitRegion.dstSubresource.layerCount = 1;
		blitRegion.dstSubresource.mipLevel = 0;

		VkBlitImageInfo2 blitInfo { .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
		blitInfo.srcImage = _ctx->viewTexture(source)._vkImage;
		blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		blitInfo.dstImage = _ctx->_swapchain->getCurrentSwapchainTexture()._vkImage;
		blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		blitInfo.filter = VK_FILTER_LINEAR;
		blitInfo.regionCount = 1;
		blitInfo.pRegions = &blitRegion;

		vkCmdBlitImage2KHR(_wrapper->_cmdBuf, &blitInfo);
	}
	void CommandBuffer::cmdTransitionSwapchainLayoutImpl(VkImageLayout newLayout) {
		VkImageMemoryBarrier2 barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
		barrier.oldLayout = _ctx->_swapchain->getCurrentSwapchainTexture()._vkCurrentImageLayout;
		barrier.newLayout = newLayout;
		barrier.subresourceRange = vkinfo::CreateImageSubresourceRange(vkutil::AspectMaskFromAttachmentLayout(newLayout));
		barrier.image = _ctx->_swapchain->getCurrentSwapchainTexture()._vkImage;
		VkDependencyInfo info = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr };
		info.imageMemoryBarrierCount = 1;
		info.pImageMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(_wrapper->_cmdBuf, &info);

		_ctx->_swapchain->_vkCurrentImageLayout = newLayout;
	}
	void CommandBuffer::cmdCopyImageImpl(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D size) {
		auto& sourceTex = _ctx->viewTexture(source);
		auto& destinationTex = _ctx->viewTexture(destination);
		ASSERT_MSG(sourceTex._vkFormat == destinationTex._vkFormat, "The images being copied must be of the same VkFormat, (format)!");
		ASSERT_MSG(sourceTex._vkExtent.width == destinationTex._vkExtent.width, "The images being copied must be of the same width!");
		ASSERT_MSG(sourceTex._vkExtent.height == destinationTex._vkExtent.height, "The images being copied must be of the same height!");

		VkImageCopy2 copyRegion = { .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2, .pNext = nullptr };

		copyRegion.extent.width = static_cast<uint32_t>(size.width);
		copyRegion.extent.height = static_cast<uint32_t>(size.height);
		copyRegion.extent.depth = 1;

		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.baseArrayLayer = 0;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.srcSubresource.mipLevel = 0;

		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.baseArrayLayer = 0;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.dstSubresource.mipLevel = 0;

		VkCopyImageInfo2 copyinfo = { .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, .pNext = nullptr };
		copyinfo.dstImage = destinationTex._vkImage;
		copyinfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		copyinfo.srcImage = sourceTex._vkImage;
		copyinfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyinfo.regionCount = 1;
		copyinfo.pRegions = &copyRegion;

		vkCmdCopyImage2KHR(_wrapper->_cmdBuf, &copyinfo);
	}
	void CommandBuffer::cmdSetDepthBiasEnableImpl(bool enable) {
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, enable ? VK_TRUE : VK_FALSE);
	}
	void CommandBuffer::cmdSetDepthBiasImpl(float constantFactor, float slopeFactor, float clamp) {
		vkCmdSetDepthBias(_wrapper->_cmdBuf, constantFactor, clamp, slopeFactor);
	}
	void CommandBuffer::cmdBindDepthStateImpl(const DepthState& state) {
		// https://github.com/corporateshark/lightweightvk/blob/master/lvk/vulkan/VulkanClasses.cpp#L2458
		const VkCompareOp op = toVulkan(state.compareOp);
		vkCmdSetDepthWriteEnable(_wrapper->_cmdBuf, state.isDepthWriteEnabled ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthTestEnable(_wrapper->_cmdBuf, (op != VK_COMPARE_OP_ALWAYS || state.isDepthWriteEnabled) ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, op);
	}
	void CommandBuffer::cmdBlitImageImpl(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize) {
		VkImageBlit2 blitRegion = { .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

		blitRegion.srcOffsets[1].x = static_cast<int32_t>(srcSize.width);
		blitRegion.srcOffsets[1].y = static_cast<int32_t>(srcSize.height);
		blitRegion.srcOffsets[1].z = 1;

		blitRegion.dstOffsets[1].x = static_cast<int32_t>(dstSize.width);
		blitRegion.dstOffsets[1].y = static_cast<int32_t>(dstSize.height);
		blitRegion.dstOffsets[1].z = 1;

		blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.srcSubresource.baseArrayLayer = 0;
		blitRegion.srcSubresource.layerCount = 1;
		blitRegion.srcSubresource.mipLevel = 0;

		blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.dstSubresource.baseArrayLayer = 0;
		blitRegion.dstSubresource.layerCount = 1;
		blitRegion.dstSubresource.mipLevel = 0;

		VkBlitImageInfo2 blitInfo = { .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
		blitInfo.srcImage = _ctx->viewTexture(source)._vkImage;
		blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		blitInfo.dstImage = _ctx->viewTexture(destination)._vkImage;
		blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		blitInfo.filter = VK_FILTER_LINEAR;
		blitInfo.regionCount = 1;
		blitInfo.pRegions = &blitRegion;

		vkCmdBlitImage2KHR(_wrapper->_cmdBuf, &blitInfo);
	}
	void CommandBuffer::cmdTransitionLayoutImpl(InternalTextureHandle source, VkImageLayout currentLayout, VkImageLayout newLayout) {
		auto& sourceTex = _ctx->viewTexture(source);
		if (sourceTex._vkCurrentImageLayout == newLayout) {
			LOG_SYSTEM(LogType::Warning, "Image ({}) is already in the requested layout.", sourceTex._debugName);
		}

		vkutil::StageAccess srcStage = vkutil::getPipelineStageAccess(currentLayout);
		vkutil::StageAccess dstStage = vkutil::getPipelineStageAccess(newLayout);

		if (sourceTex._isResolveAttachment && vkutil::IsFormatDepthOrStencil(sourceTex._vkFormat)) {
			// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#renderpass-resolve-operations
			srcStage.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			srcStage.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			dstStage.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			dstStage.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		}

		VkImageMemoryBarrier2 barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.pNext = nullptr,
				.srcStageMask = srcStage.stage,
				.srcAccessMask = srcStage.access,

				.dstStageMask = dstStage.stage,
				.dstAccessMask = dstStage.access,

				.oldLayout = currentLayout,
				.newLayout = newLayout,

				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

				.image = sourceTex._vkImage,
				.subresourceRange = vkinfo::CreateImageSubresourceRange(vkutil::AspectMaskFromFormat(sourceTex._vkFormat))
		};
		VkDependencyInfo dependency_i = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.pNext = nullptr,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrier
		};
		vkCmdPipelineBarrier2(_wrapper->_cmdBuf, &dependency_i);
		// set the texture to its new image layout
		_ctx->_texturePool.get(source)->_vkCurrentImageLayout = newLayout;
	}

	void CommandBuffer::cmdSetViewportImpl(VkExtent2D extent2D) {
		// we flip the viewport because Vulkan is reversed using LH instead of OpenGL's RH
		// HOWEVER: using Slang compilier option to reflect the y axis solves this so we dont have to flip here
		VkViewport viewport = {};
		viewport.x = 0;
		viewport.y = 0;
		viewport.width = static_cast<float>(extent2D.width);
		viewport.height = static_cast<float>(extent2D.height);
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(_wrapper->_cmdBuf, 0, 1, &viewport);
	}
	void CommandBuffer::cmdSetScissorImpl(VkExtent2D extent2D) {
		VkRect2D scissor = {};
		scissor.offset.x = 0; // default 0 for both
		scissor.offset.y = 0;
		scissor.extent = extent2D;
		vkCmdSetScissor(_wrapper->_cmdBuf, 0, 1, &scissor);
	}

//	void CommandBuffer::cmdBindDescriptorSets(std::initializer_list<InternalDescriptorSetHandle> handles) {
//		if (_isDryRun) return;
//
//		std::vector<VkDescriptorSet> dsSets = {};
//		dsSets.reserve(handles.size());
//		for (const auto& handle : handles) {
//			DescriptorSet* set = _ctx->_descriptorSetPool.get(handle);
//			dsSets.push_back(set->_vkDescriptorSet);
//		}
//
//		GraphicsPipeline* pipeline = _ctx->_graphicsPipelinePool.get(_currentPipelineHandle);
//		ASSERT_MSG(pipeline, "You must bind a graphics pipeline before binding descriptor sets!");
//		vkCmdBindDescriptorSets(_wrapper->_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->_vkPipelineLayout,
//								1,
//								dsSets.size(),
//								dsSets.data(),
//								0,
//								nullptr);
//	}

}



















