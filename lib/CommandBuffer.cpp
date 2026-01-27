//
// Created by Hayden Rivas on 10/7/25.
//


#include "CommandBuffer.h"
#include "vkinfo.h"
#include "CTX.h"
#include "vkutil.h"
#include "Logger.h"
#include "Pipelines.h"

#include <volk.h>

#include "vkstring.h"
#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#endif

#ifdef DEBUG
#define CHECK_PASS_OPERATION_MISMATCH(goalType) \
if (this->getCurrentPassType() != goalType) \
LOG_SYSTEM_NOSOURCE(LogType::Warning, \
"Calling '{}' inside a pass not of type '{}' will result in errors!", \
__func__, \
PassSourceTypeToString(goalType));
#else
#define CHECK_PASS_OPERATION_MISMATCH(goalType) (void(0))
#endif


#ifdef DEBUG
#define CHECK_SHOULD_BE_RENDERING() \
	if (!_isRendering) \
		LOG_SYSTEM_NOSOURCE(LogType::Warning, "Calling '{}' must be done between cmdBeginRendering() and cmdEndRendering()", __func__);
#else
#define CHECK_SHOULD_BE_RENDERING() (void(0))
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

#define DRY_RETURN() if (_isDryRun) return;


namespace mythril {
	static const char* PassSourceTypeToString(PassDesc::Type type) {
		switch (type) {
			case PassDesc::Type::Graphics: return "Graphics";
			case PassDesc::Type::Compute: return "Compute";
			case PassDesc::Type::Intermediate: return "Transfer";
		}
		assert(false);
	}

	PassDesc::Type CommandBuffer::getCurrentPassType() {
		if (std::holds_alternative<GraphicsPipelineHandle>(_currentPipelineHandle)) return PassDesc::Type::Graphics;
		if (std::holds_alternative<ComputePipelineHandle>(_currentPipelineHandle)) return PassDesc::Type::Compute;
		assert(false);
	}

	static void MaybeWarnPushConstantSizeMismatch(const PipelineCoreData& common, uint32_t cpuSize, std::string_view pipelineName) {
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
				LOG_SYSTEM(LogType::Warning,
					"Push constant CPU size is different than GPU size in pipeline '{}'! Your CPU structure has a size of '{}' while the shader's push constant has sizes '[{}]' respectively.",
					pipelineName,
					cpuSize,
					ss.str());
			} else if (count == kMaxWarns) {
				LOG_SYSTEM(LogType::Warning, "Push constant size mismatch for pipeline '{}' has triggered this warning >{} times and will now be silenced.", pipelineName, kMaxWarns);
			}
			count++;
		}
	}

	constexpr uint16_t kMaxColorAttachments = 16;
	// enforce that a valid CommandBuffer can only be created via this constructor
	CommandBuffer::CommandBuffer(CTX* ctx, CommandBuffer::Type type) :
	_ctx(ctx),
	_wrapper(&ctx->_imm->acquire()),
	_activePass(),
	_cmdType(type),
	_isDryRun(false) {}

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
	void CommandBuffer::cmdBeginRendering(uint32_t layerCount, uint32_t viewMask) {
		// view mask needs to be stored, as it is used during resolve
		_viewMask = viewMask;
		DRY_RETURN();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
#ifdef DEBUG
		static bool hasTriggered = false;
		if ((_currentPipelineInfo && (layerCount != 1 || viewMask != 0) && !hasTriggered)) {
			LOG_SYSTEM(LogType::Warning, "Calling cmdBeginRendering() with a specified layerCount & viewMask after cmdBind*Pipeline() is not supported, and will lead to broken rendering!");
			hasTriggered = true;
		}
#endif
		this->cmdBeginRenderingImpl(layerCount, viewMask);
	}

	void CommandBuffer::cmdEndRendering() {
		DRY_RETURN();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		this->cmdEndRenderingImpl();
	}

	void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);

		vkCmdDraw(_wrapper->_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
	}
	void CommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t baseInstance) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);

		vkCmdDrawIndexed(_wrapper->_cmdBuf, indexCount, instanceCount, firstIndex, vertexOffset, baseInstance);
	}

	void CommandBuffer::cmdDrawIndirect(const Buffer &indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		ASSERT_MSG(indirectBuffer->isIndirectBuffer(), "Buffer '{}' is not an indirect buffer, please have its usage include 'BufferUsageBits_Indirect'!", indirectBuffer->getDebugName());

		vkCmdDrawIndirect(_wrapper->_cmdBuf, indirectBuffer->_vkBuffer, offset, drawCount, stride ? stride : sizeof(VkDrawIndirectCommand));
	}

	void CommandBuffer::cmdDrawIndexedIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		ASSERT_MSG(indirectBuffer->isIndirectBuffer(), "Buffer '{}' is not an indirect buffer, please have its usage include 'BufferUsageBits_Indirect'!", indirectBuffer->getDebugName());

		vkCmdDrawIndexedIndirect(_wrapper->_cmdBuf, indirectBuffer->_vkBuffer, offset, drawCount, stride ? stride : sizeof(VkDrawIndexedIndirectCommand));
	}

	void CommandBuffer::cmdBindIndexBuffer(BufferHandle handle) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);

		const AllocatedBuffer& buffer = _ctx->view(handle);
		vkCmdBindIndexBuffer(_wrapper->_cmdBuf, buffer._vkBuffer, 0, VK_INDEX_TYPE_UINT32);
	}

	void CommandBuffer::cmdDispatchThreadGroup(const Dimensions& threadGroupCount) {
		DRY_RETURN()
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Compute);

		vkCmdDispatch(_wrapper->_cmdBuf, threadGroupCount.width, threadGroupCount.height, threadGroupCount.depth);
	}
	void CommandBuffer::cmdGenerateMipmap(TextureHandle handle) {
		DRY_RETURN()

		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Texture handle '{}' is invalid!", this->_ctx->view(handle).getDebugName());
			return;
		}
		AllocatedTexture* tex = this->_ctx->_texturePool.get(handle);
		if (tex->_numLevels <= 1) {
			LOG_SYSTEM(LogType::Warning, "Texture cannot have mipmaps when # of levels is less than 2.");
			return;
		}
		ASSERT(tex->_vkCurrentImageLayout != VK_IMAGE_LAYOUT_UNDEFINED);
		tex->generateMipmap(_wrapper->_cmdBuf);
	}

	void CommandBuffer::cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout) {
		DRY_RETURN();
		cmdTransitionLayout(source, newLayout, VkImageSubresourceRange{vkutil::AspectMaskFromFormat(_ctx->view(source).getFormat()), 0, 1, 0, 1});
	}
	void CommandBuffer::cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout, VkImageSubresourceRange range) {
		DRY_RETURN()
		cmdTransitionLayoutImpl(source, _ctx->_texturePool.get(source)->_vkCurrentImageLayout, newLayout, range);
	}

	static constexpr bool CheckTextureCopyInstead(const AllocatedTexture& source, const AllocatedTexture& destination) {
		if (source.getFormat() != destination.getFormat()) {
			return false;
		}
		if (source.getType() != destination.getType()) {
			return false;
		}
		if (source.getDimensions() != destination.getDimensions()) {
			return false;
		}
		if (source.getSampleCount() != destination.getSampleCount()) {
			return false;
		}
		return true;
	}
	void CommandBuffer::CheckTextureRenderingUsage(const AllocatedTexture& source, const AllocatedTexture& destination, const char* operation) {
		if (_isRendering) {
			VkImageView source_vkImageView = source._vkImageView;
			VkImageView dest_vkImageView = destination._vkImageView;
			for (const auto& color_attachment : this->_activePass.colorAttachments) {
				ASSERT_MSG(
						color_attachment.imageView != source_vkImageView &&
						color_attachment.imageView != dest_vkImageView,
						"You cannot {} an image currently being used by a RenderPass!", operation);
			}
			if (this->_activePass.depthAttachment.has_value()) {
				const auto& depth_attachment = this->_activePass.depthAttachment.value();
				ASSERT_MSG(
						depth_attachment.imageView != source_vkImageView &&
						depth_attachment.imageView != dest_vkImageView,
						"You cannot {} an image currently being used by a RenderPass!", operation);
			}
		}
	}
	void CommandBuffer::CheckImageLayoutAuto(TextureHandle sourceHandle, TextureHandle destinationHandle, const char* operation) {
		auto& sourceObject = _ctx->view(sourceHandle);
		auto& destinationObject = _ctx->view(destinationHandle);
		if (sourceObject.getImageLayout() != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			LOG_SYSTEM(LogType::Info, "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) before {}. Was in ({})", sourceObject._debugName, operation, vkstring::VulkanImageLayoutToString(sourceObject.getImageLayout()));
			this->cmdTransitionLayout(sourceHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		if (destinationObject.getImageLayout() != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			LOG_SYSTEM(LogType::Info, "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) before {}. Was in ({})", destinationObject._debugName, operation, vkstring::VulkanImageLayoutToString(destinationObject.getImageLayout()));
			this->cmdTransitionLayout(destinationHandle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}
	}
	void CommandBuffer::cmdBlitImageToSwapchain(TextureHandle source) {
		DRY_RETURN()
		const TextureHandle destination = this->_ctx->getCurrentSwapchainTexHandle();
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");
		auto &sourceTex = _ctx->view(source);
		auto &destinationTex = _ctx->view(destination);
#ifdef DEBUG
		// check if textures are being rendered to
		CheckTextureRenderingUsage(sourceTex, destinationTex, "blit");
		// check if copy could have been used instead
		if (CheckTextureCopyInstead(sourceTex, destinationTex)) {
			LOG_SYSTEM(LogType::Suggestion, "Usage of cmdBlitImage can be replaced by cmdCopyImage between src: '{}' and dest: '{}'", sourceTex.getDebugName(), destinationTex.getDebugName());
		}
#endif
		ASSERT_MSG(sourceTex.getSampleCount() == VK_SAMPLE_COUNT_1_BIT, "You cannot blit a image with more than 1 samples.");
		CheckImageLayoutAuto(source, destination, "blitting");
		cmdBlitImageImpl(source, destination, sourceTex.getExtentAs2D(), destinationTex.getExtentAs2D());
	}
	void CommandBuffer::cmdCopyImageToSwapchain(TextureHandle source) {
		DRY_RETURN()
		const TextureHandle destination = this->_ctx->getCurrentSwapchainTexHandle();
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");

		auto &sourceTex = _ctx->view(source);
		auto &destinationTex = _ctx->view(destination);
#ifdef DEBUG
		CheckTextureRenderingUsage(sourceTex, destinationTex, "copy");
#endif
		CheckImageLayoutAuto(source, destination, "copying");
		// needs to be same so doesnt matter if we take source or destination size
		cmdCopyImageImpl(source, destination, _ctx->view(source).getExtentAs2D());
	}


	void CommandBuffer::cmdBlitImage(TextureHandle source, TextureHandle destination) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");

		const AllocatedTexture& sourceTex = _ctx->view(source);
		const AllocatedTexture& destinationTex = _ctx->view(destination);
#ifdef DEBUG
		// check if textures are being rendered to
		CheckTextureRenderingUsage(sourceTex, destinationTex, "blit");
		// check if copy could have been used instead
		if (CheckTextureCopyInstead(sourceTex, destinationTex)) {
			LOG_SYSTEM(LogType::Suggestion, "Usage of cmdBlitImage can be replaced by cmdCopyImage between src: '{}' and dest: '{}'", sourceTex.getDebugName(), destinationTex.getDebugName());
		}
#endif
		ASSERT_MSG(sourceTex.getSampleCount() == VK_SAMPLE_COUNT_1_BIT, "You cannot blit a image with more than 1 samples.");
		CheckImageLayoutAuto(source, destination, "blitting");
		cmdBlitImageImpl(source, destination, sourceTex.getExtentAs2D(), destinationTex.getExtentAs2D());
	}
	void CommandBuffer::cmdCopyImage(TextureHandle source, TextureHandle destination) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");

		auto &sourceTex = _ctx->view(source);
		auto &destinationTex = _ctx->view(destination);
#ifdef DEBUG
		CheckTextureRenderingUsage(sourceTex, destinationTex, "copy");
#endif
		CheckImageLayoutAuto(source, destination, "copying");
		// needs to be same so doesnt matter if we take source or destination size
		cmdCopyImageImpl(source, destination, _ctx->view(source).getExtentAs2D());
	}

	void CommandBuffer::cmdPushConstants(const void* data, uint32_t size, uint32_t offset) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		if (!_currentPipelineInfo) {
			LOG_SYSTEM(LogType::Warning, "No pipeline currently bound, will not perform push constants!");
			return;
		}

		ASSERT_MSG(size % 4 == 0, "Push constant size needs to be a multiple of 4. Is size {}", size);
		const VkPhysicalDeviceLimits& limits = _ctx->getPhysicalDeviceProperties10().limits;
		if (size + offset > limits.maxPushConstantsSize) {
			LOG_SYSTEM(LogType::Error, "Push constants size exceeded %u (max %u bytes)", size + offset, limits.maxPushConstantsSize);
		}

		// we just use the enum for PassSource::Type even though it makes no sense cause thats not what we are testing for
		const PassDesc::Type type = getCurrentPassType();
		const VkShaderStageFlags stages = [](const PassDesc::Type passType) -> VkShaderStageFlags {
			switch (passType) {
				case PassDesc::Type::Graphics: return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
				case PassDesc::Type::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
				default: assert(false);
			}
		}(type);
#ifdef DEBUG
		MaybeWarnPushConstantSizeMismatch(_currentPipelineInfo->core, size, _currentPipelineInfo->debugName);
#endif
		vkCmdPushConstants(_wrapper->_cmdBuf, _currentPipelineInfo->core._vkPipelineLayout, static_cast<VkShaderStageFlagBits>(stages), offset, size, data);
	}

	void CommandBuffer::cmdBindPipelineImpl(const PipelineCoreData* common, VkPipelineBindPoint bindPoint) {
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

	void CommandBuffer::cmdBindGraphicsPipeline(GraphicsPipelineHandle handle) {
		ASSERT(this->_ctx);
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded render pipeline was invalid/empty!");
			return;
		}
		AllocatedGraphicsPipeline* pipeline = _ctx->_graphicsPipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (_isDryRun) {
			if (pipeline->_shared.core._vkPipeline != VK_NULL_HANDLE) {
				// LOG_SYSTEM(LogType::Error, "Dry run attempting to resolve Pipeline '{}' that has already been built!", pipeline->_debugName);
				return;
			}
			// we perform construction inside our dry run for all pipelines, which is when we compile the RenderGraph
			// we do this so we dont stutter mid gameplay loop
			_ctx->resolveGraphicsPipelineImpl(*pipeline, _viewMask);
			return;
		}
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		this->_currentPipelineHandle = handle;
		this->_currentPipelineInfo = &pipeline->_shared;

		// alias it
		const SharedPipelineInfo* info = this->_currentPipelineInfo;
		CHECK_PIPELINE_REBIND(&info->core, _lastBoundvkPipeline, info->debugName);
		this->cmdBindPipelineImpl(&info->core, VK_PIPELINE_BIND_POINT_GRAPHICS);
	}

	void CommandBuffer::cmdBindComputePipeline(ComputePipelineHandle handle) {
		ASSERT(this->_ctx);
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded compute pipeline was invalid/empty!");
			return;
		}
		AllocatedComputePipeline* pipeline = _ctx->_computePipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (_isDryRun) {
			if (pipeline->_shared.core._vkPipeline != VK_NULL_HANDLE) {
				// LOG_SYSTEM(LogType::Error, "Dry run attempting to resolve Pipeline '{}' that has already been built!", pipeline->getDebugName());
				return;
			}
			_ctx->resolveComputePipelineImpl(*pipeline);
			return;
		}
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		this->_currentPipelineHandle = handle;
		this->_currentPipelineInfo = &pipeline->_shared;
		const SharedPipelineInfo* info = this->_currentPipelineInfo;
		CHECK_PIPELINE_REBIND(&info->core, _lastBoundvkPipeline, info->debugName);
		this->cmdBindPipelineImpl(&info->core, VK_PIPELINE_BIND_POINT_COMPUTE);
	}

	// current issues with host buffer
	void CommandBuffer::cmdUpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		ASSERT_MSG(!_isRendering, "You cannot update a buffer while rendering! Please move this command either before or after rendering.");
		ASSERT(handle.valid());
		ASSERT_MSG(size && size <= 65536, "You cannot call cmdUpdateBuffer with a size <= 65'536 (64kb)");
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
	void CommandBuffer::cmdCopyImageToBuffer(TextureHandle source, BufferHandle destination, const VkBufferImageCopy& region) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		auto& sourceTex = _ctx->view(source);
		VkImageLayout currentLayout = sourceTex._vkCurrentImageLayout;
		if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && currentLayout != VK_IMAGE_LAYOUT_GENERAL) {
			cmdTransitionLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		VkImageLayout properLayout = sourceTex._vkCurrentImageLayout;
		vkCmdCopyImageToBuffer(_wrapper->_cmdBuf, sourceTex._vkImage, properLayout, _ctx->view(destination)._vkBuffer, 1, &region);
	}
	void CommandBuffer::cmdBindDepthState(const DepthState& state) {
		DRY_RETURN()
		// https://github.com/corporateshark/lightweightvk/blob/master/lvk/vulkan/VulkanClasses.cpp#L2458
		const VkCompareOp op = toVulkan(state.compareOp);
		vkCmdSetDepthWriteEnable(_wrapper->_cmdBuf, state.isDepthWriteEnabled ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthTestEnable(_wrapper->_cmdBuf, (op != VK_COMPARE_OP_ALWAYS || state.isDepthWriteEnabled) ? VK_TRUE : VK_FALSE);
		vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, op);
	}
	void CommandBuffer::cmdSetDepthBiasEnable(bool enable) {
		DRY_RETURN();
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, enable ? VK_TRUE : VK_FALSE);
	}
	void CommandBuffer::cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp) {
		DRY_RETURN();
		vkCmdSetDepthBias(_wrapper->_cmdBuf, constantFactor, clamp, slopeFactor);
	}

	// void CommandBuffer::cmdClearColorImage(InternalTextureHandle texture, const ClearColor& color) {
	// 	DRY_RETURN();
	// 	AllocatedTexture& texture = _ctx->_texturePool.get(texture);
	// 	const VkImageSubresourceRange range = {
	// 		.aspectMask = texture.getImageAspectFlags(),
	// 		.baseArrayLayer = texture._
	// 	};
	// 	vkCmdClearColorImage(_wrapper->_cmdBuf, texture._vkImage, texture.getImageLayout(), color.getAsVkClearColorValue(), 1, VkImageSubresourceRange{});
	// }
	//
	// void CommandBuffer::cmdClearDepthStencilImage(InternalTextureHandle texture, const ClearDepthStencil &value) {
	// 	DRY_RETURN();
	// 	vkCmdClearDepthStencilImage(_wrapper->_cmdBuf, )
	// }


#ifdef MYTH_ENABLED_IMGUI
	void CommandBuffer::cmdDrawImGui() {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		CHECK_SHOULD_BE_RENDERING();
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
	void CommandBuffer::cmdBeginRenderingImpl(uint32_t layerCount, uint32_t viewMask) {
		ASSERT_MSG(!_isRendering, "Command Buffer is already rendering!");
		const bool hasDepth = _activePass.depthAttachment.has_value();
		const VkRect2D renderArea = _activePass.renderArea;
		std::vector<VkRenderingAttachmentInfo> vk_color_attachment_infos{};
		vk_color_attachment_infos.reserve(_activePass.colorAttachments.size());
		for (const auto& attachment_info : _activePass.colorAttachments) {
			vk_color_attachment_infos.push_back(attachment_info.getAsVkRenderingAttachmentInfo());
		}
		VkRenderingAttachmentInfo vk_depth_attachment_info{};
		if (hasDepth) {
			vk_depth_attachment_info = _activePass.depthAttachment->getAsVkRenderingAttachmentInfo();
		}
		const VkRenderingInfo info = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea = renderArea,
			.layerCount = layerCount,
			.viewMask = viewMask,
			.colorAttachmentCount = static_cast<uint32_t>(_activePass.colorAttachments.size()),
			.pColorAttachments = vk_color_attachment_infos.data(),
			.pDepthAttachment = hasDepth ? &vk_depth_attachment_info : nullptr,
			.pStencilAttachment = nullptr
		};
		cmdSetViewportImpl(renderArea.extent);
		cmdSetScissorImpl(renderArea.extent);
		cmdBindDepthState({});

		_ctx->checkAndUpdateBindlessDescriptorSetImpl();

		vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, VK_COMPARE_OP_ALWAYS);
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, VK_FALSE);
		vkCmdBeginRendering(_wrapper->_cmdBuf, &info);
		_isRendering = true;
	}

	void CommandBuffer::cmdEndRenderingImpl() {
		ASSERT_MSG(_isRendering, "Command buffer isnt even rendering!");
		vkCmdEndRendering(_wrapper->_cmdBuf);
		_isRendering = false;
	}
	void CommandBuffer::bufferBarrierImpl(BufferHandle bufhandle, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
		auto& buf = _ctx->view(bufhandle);

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
	void CommandBuffer::cmdCopyImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D size) {
		auto& sourceTex = _ctx->view(source);
		auto& destinationTex = _ctx->view(destination);
		ASSERT_MSG((sourceTex._vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) > 0, "Source image must have VK_IMAGE_USAGE_TRANSFER_SRC_BIT!");
		ASSERT_MSG((destinationTex._vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) > 0, "Destination image must have VK_IMAGE_USAGE_TRANSFER_DST_BIT!");
		ASSERT_MSG(sourceTex._numLayers == destinationTex._numLayers, "The images being copied must have the same number of layers!");
		ASSERT_MSG(sourceTex._vkFormat == destinationTex._vkFormat, "The images being copied must be of the same VkFormat (source is '{}' while destination is '{}')!",
			vkstring::VulkanFormatToString(sourceTex.getFormat()),
			vkstring::VulkanFormatToString(destinationTex.getFormat()));
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

		vkCmdCopyImage2(_wrapper->_cmdBuf, &copyinfo);
	}
	void CommandBuffer::cmdBlitImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize) {
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
		blitInfo.srcImage = _ctx->view(source)._vkImage;
		blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		blitInfo.dstImage = _ctx->view(destination)._vkImage;
		blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		blitInfo.filter = VK_FILTER_NEAREST; // todo allow customization
		blitInfo.regionCount = 1;
		blitInfo.pRegions = &blitRegion;

		vkCmdBlitImage2(_wrapper->_cmdBuf, &blitInfo);
	}
	void CommandBuffer::cmdTransitionLayoutImpl(TextureHandle source, VkImageLayout currentLayout, VkImageLayout newLayout, VkImageSubresourceRange range) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		auto& sourceTex = _ctx->view(source);
		// if (sourceTex._vkCurrentImageLayout == newLayout) {
		// 	LOG_SYSTEM(LogType::Info, "Image ({}) is already in the requested layout.", sourceTex._debugName);
		// }

		vkutil::StageAccess srcStage = vkutil::GetPipelineStageAccess(currentLayout);
		vkutil::StageAccess dstStage = vkutil::GetPipelineStageAccess(newLayout);
		if (sourceTex.isSwapchainImage()) {
			srcStage = {
				.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.access = VK_ACCESS_2_MEMORY_WRITE_BIT
			};
			dstStage = {
				.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.access = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT
			};
		}
		if (sourceTex._isResolveAttachment && vkutil::IsFormatDepthOrStencil(sourceTex._vkFormat)) {
			// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#renderpass-resolve-operations
			srcStage.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			srcStage.stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			dstStage.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			dstStage.access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		}

		vkutil::ImageMemoryBarrier2(_wrapper->_cmdBuf, sourceTex._vkImage, srcStage, dstStage, currentLayout, newLayout, range);
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



















