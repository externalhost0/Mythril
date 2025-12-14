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

#ifdef DEBUG
#define CHECK_PASS_OPERATION_MISMATCH(goalType)               \
if (this->getCurrentPassType() != goalType)                        \
LOG_SYSTEM_NOSOURCE(LogType::Warning,                              \
"Calling '{}' inside a pass not of type '{}' is not reccomended!", \
__func__,                                                          \
PassSourceTypeToString(goalType));
#else
#define CHECK_PASS_OPERATION_MISMATCH(goalType) (void(0))
#endif

#ifdef DEBUG
#define CHECK_PIPELINE_REBIND(common, lastBound, debugName) \
	do { \
		if ((common)->_vkPipeline == (lastBound)) { \
			static std::unordered_map<VkPipeline, int> g_rebindWarningCount; \
			int& count = g_rebindWarningCount[(common)->_vkPipeline]; \
			static constexpr unsigned int kMaxWarns = 5; \
			if (count < kMaxWarns) { \
				LOG_SYSTEM(LogType::Warning, "Called to rebind already bound pipeline '{}'", debugName); \
			} else if (count == kMaxWarns) { \
				LOG_SYSTEM(LogType::Warning, "Pipeline '{}' has triggered this warning >{} times and will now be silenced.", debugName, kMaxWarns); \
			} \
			count++; \
			return; \
		} \
	} while(0)
#else
#define CHECK_PIPELINE_REBIND(common, lastBound, debugName) \
	do { \
		if ((common)->_vkPipeline == (lastBound)) { \
			return; \
		} \
	} while(0)
#endif


namespace mythril {
	static const char* PassSourceTypeToString(PassSource::Type type) {
		switch (type) {
			case PassSource::Type::Graphics: return "Graphics";
			case PassSource::Type::Compute: return "Compute";
		}
	}

	PassSource::Type CommandBuffer::getCurrentPassType() {
		if (std::holds_alternative<InternalGraphicsPipelineHandle>(_currentPipelineHandle)) return PassSource::Type::Graphics;
		else if (std::holds_alternative<InternalComputePipelineHandle>(_currentPipelineHandle)) return PassSource::Type::Compute;
		assert(false);
	}

	static void MaybeWarnPushConstantSizeMismatch(PipelineCommon& common, uint32_t cpuSize) {
		static std::unordered_map<VkPipeline, int> g_pushConstantWarningCount;
		int& count = g_pushConstantWarningCount[common._vkPipeline];
		static constexpr unsigned int kMaxWarns = 5;

		bool matches = false;
		for (const VkPushConstantRange& push : common.signature.pushes) {
			if (push.size == cpuSize) {
				matches = true;
				break;
			}
		}
		if (!matches) {
			if (count < kMaxWarns) {
				std::ostringstream ss;
				bool first = true;
				for (const VkPushConstantRange& push : common.signature.pushes) {
					if (!first) ss << ", ";
					first = false;
					ss << push.size;
				}
				LOG_SYSTEM(LogType::Warning, "Push constant CPU size is different than GPU size! Your CPU structure has a size of '{}' while the shader's push constant has sizes '[{}]' respectively.", cpuSize, ss.str());
			} else if (count == kMaxWarns) {
				LOG_SYSTEM(LogType::Warning, "Push constant size mismatch for pipeline has triggered this warning >{} times and will now be silenced.", kMaxWarns);
			}
			count++;
		}
	}

	constexpr uint16_t kMaxColorAttachments = 16;
	// enforce that a valid CommandBuffer can only be created via this constructor
	CommandBuffer::CommandBuffer(CTX* ctx, CommandBuffer::Type type) : _ctx(ctx), _wrapper(&ctx->_imm->acquire()), _cmdType(type), _isDryRun(false) {};
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
		CHECK_PASS_OPERATION_MISMATCH(PassSource::Type::Graphics);

		vkCmdDraw(_wrapper->_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
	}
	void CommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t baseInstance) {
		if (_isDryRun) return;
		CHECK_PASS_OPERATION_MISMATCH(PassSource::Type::Graphics);

		vkCmdDrawIndexed(_wrapper->_cmdBuf, indexCount, instanceCount, firstIndex, vertexOffset, baseInstance);
	}

	void CommandBuffer::cmdBindIndexBuffer(InternalBufferHandle handle) {
		if (_isDryRun) return;
		CHECK_PASS_OPERATION_MISMATCH(PassSource::Type::Graphics);

		const AllocatedBuffer& buffer = _ctx->viewBuffer(handle);
		vkCmdBindIndexBuffer(_wrapper->_cmdBuf, buffer._vkBuffer, 0, VK_INDEX_TYPE_UINT32);
	}

	void CommandBuffer::cmdDispatchThreadGroup(const Dimensions& threadGroupCount) {
		if (_isDryRun) return;
		CHECK_PASS_OPERATION_MISMATCH(PassSource::Type::Compute);

		vkCmdDispatch(_wrapper->_cmdBuf, threadGroupCount.width, threadGroupCount.height, threadGroupCount.depth);
	}

	void CommandBuffer::cmdTransitionLayout(InternalTextureHandle source, VkImageLayout newLayout) {
		if (_isDryRun) return;

		cmdTransitionLayoutImpl(source, _ctx->viewTexture(source)._vkCurrentImageLayout, newLayout);
	}
	void CommandBuffer::cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination) {
		if (_isDryRun) return;
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");

#ifdef DEBUG
		VkImageView source_vkImageView = _ctx->viewTexture(source)._vkImageView;
		VkImageView dest_vkImageView = _ctx->viewTexture(destination)._vkImageView;
		for (const auto& color_attachment : this->_activePass.colorAttachments) {
			ASSERT_MSG(
					color_attachment.imageView != source_vkImageView &&
					color_attachment.imageView != dest_vkImageView,
					"You cannot blit an image currently being used by a RenderPass!");
		}
		if (this->_activePass.depthAttachment.has_value()) {
			const auto& depth_attachment = this->_activePass.depthAttachment.value();
			ASSERT_MSG(
					depth_attachment.imageView != source_vkImageView &&
					depth_attachment.imageView != dest_vkImageView,
					"You cannot blit an image currently being used by a RenderPass!");
		}

#endif
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
		if (!_currentPipelineCommon) {
			LOG_SYSTEM(LogType::Warning, "No pipeline currently bound, will not perform push constants!");
			return;
		}

		ASSERT_MSG(size % 4 == 0, "Push constant size needs to be a multiple of 4. Is size {}", size);
		const VkPhysicalDeviceLimits& limits = _ctx->_vkPhysDeviceProperties.limits;
		if (size + offset > limits.maxPushConstantsSize) {
			LOG_SYSTEM(LogType::Error, "Push constants size exceeded %u (max %u bytes)", size + offset, limits.maxPushConstantsSize);
		}

		// we just use the enum for PassSource::Type even though it makes no sense cause thats not what we are testing for
		PassSource::Type type = getCurrentPassType();
		VkShaderStageFlags stages;
		switch (type) {
			case PassSource::Type::Compute: {
				stages = VK_SHADER_STAGE_COMPUTE_BIT;
			} break;
			case PassSource::Type::Graphics: {
				stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			}
		}
#ifdef DEBUG
		MaybeWarnPushConstantSizeMismatch(*_currentPipelineCommon, size);
#endif
		vkCmdPushConstants(_wrapper->_cmdBuf, _currentPipelineCommon->_vkPipelineLayout, static_cast<VkShaderStageFlagBits>(stages), offset, size, data);
	}

	void CommandBuffer::cmdBindPipelineImpl(const PipelineCommon *common, VkPipelineBindPoint bindPoint) {
		// only bind pipeline if its not currently bound
		// what we should be actually doing anyways
		_lastBoundvkPipeline = common->_vkPipeline;
		vkCmdBindPipeline(_wrapper->_cmdBuf, bindPoint, common->_vkPipeline);
		if (common->_vkBindableDescriptorSets.empty()) return;
		vkCmdBindDescriptorSets(_wrapper->_cmdBuf, bindPoint, common->_vkPipelineLayout,
								0,
								common->_vkBindableDescriptorSets.size(),
								common->_vkBindableDescriptorSets.data(),
								0,
								nullptr);
	}

	void CommandBuffer::cmdBindGraphicsPipeline(InternalGraphicsPipelineHandle handle) {
		ASSERT(this->_ctx);

		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded render pipeline was invalid/empty!");
			return;
		}
		GraphicsPipeline* pipeline = _ctx->_graphicsPipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (_isDryRun) {
			if (pipeline->_common._vkPipeline) {
				LOG_SYSTEM(LogType::Error, "Dry run attempting to resolve Pipeline '{}' that has already been built!", pipeline->_debugName);
				return;
			}
			// we perform construction inside our dry run for all pipelines, which is when we compile the RenderGraph
			// we do this so we dont stutter mid gameplay loop
			_ctx->resolveGraphicsPipelineImpl(*pipeline);
			return;
		}
		this->_currentPipelineHandle = handle;
		this->_currentPipelineCommon = &pipeline->_common;
		const PipelineCommon* common = this->_currentPipelineCommon;
		CHECK_PIPELINE_REBIND(common, _lastBoundvkPipeline, pipeline->_debugName);
		this->cmdBindPipelineImpl(common, VK_PIPELINE_BIND_POINT_GRAPHICS);
	}

	void CommandBuffer::cmdBindComputePipeline(InternalComputePipelineHandle handle) {
		ASSERT(this->_ctx);

		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded compute pipeline was invalid/empty!");
			return;
		}
		ComputePipeline* pipeline = _ctx->_computePipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (_isDryRun) {
			if (pipeline->_common._vkPipeline) {
				LOG_SYSTEM(LogType::Error, "Dry run attempting to resolve Pipeline '{}' that has already been built!", pipeline->_debugName);
				return;
			}
			_ctx->resolveComputePipelineImpl(*pipeline);
			return;
		}
		this->_currentPipelineHandle = handle;
		this->_currentPipelineCommon = &pipeline->_common;
		const PipelineCommon* common = this->_currentPipelineCommon;
		CHECK_PIPELINE_REBIND(common, _lastBoundvkPipeline, pipeline->_debugName);
		this->cmdBindPipelineImpl(common, VK_PIPELINE_BIND_POINT_COMPUTE);
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
	void CommandBuffer::cmdBindDepthState(const DepthState& state) {
		if (_isDryRun) return;
		// https://github.com/corporateshark/lightweightvk/blob/master/lvk/vulkan/VulkanClasses.cpp#L2458
		const VkCompareOp op = toVulkan(state.compareOp);
		vkCmdSetDepthWriteEnable(_wrapper->_cmdBuf, state.isDepthWriteEnabled ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthTestEnable(_wrapper->_cmdBuf, (op != VK_COMPARE_OP_ALWAYS || state.isDepthWriteEnabled) ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, op);
	}

#ifdef MYTH_ENABLED_IMGUI
	void CommandBuffer::cmdDrawImGui() {
		if (_isDryRun) return;
#ifdef DEBUG
		for (const auto& color_attachment : this->_activePass.colorAttachments) {
			ASSERT_MSG(color_attachment.resolveImageView == VK_NULL_HANDLE, "Rendering of ImGui cannot be done inside a multisampled texture!");
			ASSERT_MSG(color_attachment.imageFormat == this->_ctx->_imguiPlugin.getFormat(), "ImGui is drawing to a format that it was not given! Please pass the VkFormat of the texture which ImGui is drawing to when calling 'withImGuiPlugin()'!");
		}
#endif
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
		cmdSetViewportImpl(renderExtent);
		cmdSetScissorImpl(renderExtent);
		cmdBindDepthState({});

		_ctx->checkAndUpdateBindlessDescriptorSetImpl();

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
		blitInfo.filter = VK_FILTER_NEAREST; // todo allow customization
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

}



















