//
// Created by Hayden Rivas on 10/7/25.
//


#include "CommandBuffer.h"
#include "mythril/CTX.h"
#include "Logger.h"
#include "Pipelines.h"
#include "vkinfo.h"
#include "vkutil.h"

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
	using namespace detail;
	static const char* PassSourceTypeToString(PassDesc::Type type) {
		switch (type) {
			case PassDesc::Type::Graphics:
				return "Graphics";
			case PassDesc::Type::Compute:
				return "Compute";
			case PassDesc::Type::Intermediate:
				return "Transfer";
			case PassDesc::Type::RayTracing:
				return "RayTracing";
			default:
				assert(false);
		}
	}

	PassDesc::Type CommandBuffer::getCurrentPassType() const {
		if (std::holds_alternative<GraphicsPipelineHandle>(_currentPipelineHandle))
			return PassDesc::Type::Graphics;
		if (std::holds_alternative<ComputePipelineHandle>(_currentPipelineHandle))
			return PassDesc::Type::Compute;
		if (std::holds_alternative<RayTracingPipelineHandle>(_currentPipelineHandle))
			return PassDesc::Type::RayTracing;
		ASSERT_MSG(false, "Catastrophic error, the developer forgot to account for other variants in the handle!");
	}

	static void MaybeWarnPushConstantSizeMismatch(const PipelineCoreData& common, uint32_t cpuSize, std::string_view pipelineName) {
		static std::unordered_map<VkPipeline, int> g_pushConstantWarningCount;
		int& count = g_pushConstantWarningCount[common._vkPipeline];
		static constexpr unsigned int kMaxWarns = 5;

		bool matches = false;
		for (const VkPushConstantRange& push: common.signature.pushes) {
			if (push.size == cpuSize) {
				matches = true;
				break;
			}
		}
		if (!matches) {
			if (count < kMaxWarns) {
				std::ostringstream ss;
				bool first = true;
				for (const VkPushConstantRange& push: common.signature.pushes) {
					if (!first)
						ss << ", ";
					first = false;
					ss << push.size;
				}
				LOG_SYSTEM(
				        LogType::Warning,
				        "Push constant CPU size is different than GPU size in pipeline '{}'! Your CPU structure has a size of '{}' while the shader's push constant has sizes '[{}]' respectively.",
				        pipelineName,
				        cpuSize,
				        ss.str()
				);
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
	    _wrapper(&ctx->_immGraphics->acquire()),
	    _activePass(),
	    _cmdType(type),
	    _isDryRun(false) {}

	CommandBuffer::~CommandBuffer() { ASSERT_MSG(!_isRendering, "Please call to end rendering before destroying a Command Buffer!"); }
	VkCommandBufferSubmitInfo CommandBuffer::requestSubmitInfo() const {
		VkCommandBufferSubmitInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		info.pNext = nullptr;
		info.commandBuffer = _wrapper->_cmdBuf;
		info.deviceMask = 0;
		return info;
	}

	// currently should not be called as begin/end rendering is managed by the rendergraph instead
	void CommandBuffer::cmdBeginRenderingEXT(uint32_t layerCount, uint32_t viewMask) {
		// view mask needs to be stored, as it is used during resolve
		_viewMask = viewMask;
		DRY_RETURN();
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
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

	void CommandBuffer::cmdEndRenderingEXT() {
		DRY_RETURN();
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		this->cmdEndRenderingImpl();
	}

	// ALL PUBLIC COMMANDS AVAILBLE TO USER //
	void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDraw()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);

		vkCmdDraw(_wrapper->_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
	}
	void CommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t baseInstance) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDrawIndexed()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);

		vkCmdDrawIndexed(_wrapper->_cmdBuf, indexCount, instanceCount, firstIndex, vertexOffset, baseInstance);
	}

	void CommandBuffer::cmdDrawIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		const AllocatedBuffer& buf = _ctx->view(indirectBuffer.handle());
		ASSERT_MSG(buf.isIndirectBuffer(), "Buffer '{}' is not an indirect buffer, please have its usage include 'BufferUsageBits_Indirect'!", buf.getDebugName());
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDrawIndirect()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);

		vkCmdDrawIndirect(_wrapper->_cmdBuf, buf._vkBuffer, offset, drawCount, stride ? stride : sizeof(VkDrawIndirectCommand));
	}

	void CommandBuffer::cmdDrawIndexedIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		const AllocatedBuffer& buf = _ctx->view(indirectBuffer.handle());
		ASSERT_MSG(buf.isIndirectBuffer(), "Buffer '{}' is not an indirect buffer, please have its usage include 'BufferUsageBits_Indirect'!", buf.getDebugName());
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDrawIndexedIndirect()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);

		vkCmdDrawIndexedIndirect(_wrapper->_cmdBuf, buf._vkBuffer, offset, drawCount, stride ? stride : sizeof(VkDrawIndexedIndirectCommand));
	}

	void CommandBuffer::cmdBindIndexBuffer(BufferHandle handle) {
		DRY_RETURN();
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		const AllocatedBuffer& buffer = _ctx->view(handle);
		// ensure that all index buffers created are of type VK_INDEX_TYPE_UINT32
		vkCmdBindIndexBuffer(_wrapper->_cmdBuf, buffer._vkBuffer, 0, VK_INDEX_TYPE_UINT32);
	}

	void CommandBuffer::cmdDispatchThreadGroup(const Dimensions& threadGroupCount) {
		DRY_RETURN()
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Compute);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		orderAfterPreviousDispatchImpl();
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDispatchThreadGroup()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);

		vkCmdDispatch(_wrapper->_cmdBuf, threadGroupCount.width, threadGroupCount.height, threadGroupCount.depth);
	}
	void CommandBuffer::orderAfterPreviousDispatchImpl() {
		const bool needsBarrier = _hasDispatchedInPass && _activePass && _activePass->inOrderBarrier.has_value();
		_hasDispatchedInPass = true;
		if (!needsBarrier)
			return;
		const VkDependencyInfo dependencyInfo = {
		    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		    .memoryBarrierCount = 1,
		    .pMemoryBarriers = &_activePass->inOrderBarrier.value(),
		};
		vkCmdPipelineBarrier2(_wrapper->_cmdBuf, &dependencyInfo);
	}
	void CommandBuffer::cmdTraceRays(const Dimensions& queryDimension) {
		DRY_RETURN()
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::RayTracing);
		if (!_ctx->isExtensionEnabled(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
			LOG_SYSTEM(LogType::Warning, "You are calling cmdTraceRays without VK_KHR_ray_tracing_pipeline extension enabled.");
			return;
		}
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		orderAfterPreviousDispatchImpl();
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdTraceRays()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		AllocatedRayTracingPipeline* rtpipeline = this->_ctx->_rayTracingPipelinePool.get(std::get<RayTracingPipelineHandle>(_currentPipelineHandle));
		vkCmdTraceRaysKHR(_wrapper->_cmdBuf,
			&rtpipeline->_sbtEntryRayGen,
			&rtpipeline->_sbtEntryMiss,
			&rtpipeline->_sbtEntryHit,
			&rtpipeline->_sbtEntryCallable,
			queryDimension.width, queryDimension.height, queryDimension.depth);
	}

	void CommandBuffer::cmdGenerateMipmap(TextureHandle handle) {
		DRY_RETURN()
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Intermediate);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Texture handle '{}' is invalid!", this->_ctx->view(handle).getDebugName());
			return;
		}
		AllocatedTexture* tex = this->_ctx->_texturePool.get(handle);
		if (tex->_numLevels <= 1) {
			LOG_SYSTEM(LogType::Warning, "Texture cannot have mipmaps when # of levels is less than 2.");
			return;
		}
		tex->generateMipmap(_wrapper->_cmdBuf);
	}

	void CommandBuffer::cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout) {
		DRY_RETURN();
		this->cmdTransitionLayout(source, newLayout, VkImageSubresourceRange{vkutil::AspectMaskFromFormat(_ctx->view(source).getFormat()), 0, 1, 0, 1});
	}
	void CommandBuffer::cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout, VkImageSubresourceRange range) {
		DRY_RETURN()
		AllocatedTexture* tex = _ctx->_texturePool.get(source);
		tex->transitionLayout(_wrapper->_cmdBuf, newLayout, tex->toSubresourceRange(range));
	}

	static bool CheckTextureCopyInstead(const AllocatedTexture& source, const AllocatedTexture& destination) {
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
	void CommandBuffer::CheckTextureRenderingUsage(const AllocatedTexture& source, const AllocatedTexture& destination, const char* operation) const {
		if (_isRendering) {
			VkImageView source_vkImageView = source._vkImageView;
			VkImageView dest_vkImageView = destination._vkImageView;
			for (const auto& color_attachment: this->_activePass->colorAttachments) {
				ASSERT_MSG(
				        color_attachment.imageView != source_vkImageView && color_attachment.imageView != dest_vkImageView, "You cannot {} an image currently being used by a RenderPass!", operation
				);
			}
			if (this->_activePass->depthAttachment.has_value()) {
				const auto& depth_attachment = this->_activePass->depthAttachment.value();
				ASSERT_MSG(
				        depth_attachment.imageView != source_vkImageView && depth_attachment.imageView != dest_vkImageView, "You cannot {} an image currently being used by a RenderPass!", operation
				);
			}
		}
	}
	void CommandBuffer::CheckImageLayoutAuto(TextureHandle sourceHandle, TextureHandle destinationHandle, const char* operation) {
		auto& sourceObject = _ctx->view(sourceHandle);
		auto& destinationObject = _ctx->view(destinationHandle);
		constexpr SubresourceRange baseSubresource = {0, 1, 0, 1};
		if (sourceObject.getImageLayout(baseSubresource) != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
			LOG_SYSTEM(
			        LogType::Info,
			        "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) before {}. Was in ({})",
			        sourceObject._debugName,
			        operation,
			        vkstring::VulkanImageLayoutToString(sourceObject.getImageLayout(baseSubresource))
			);
			this->cmdTransitionLayout(sourceHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		if (destinationObject.getImageLayout(baseSubresource) != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			LOG_SYSTEM(
			        LogType::Info,
			        "Automatically resolved texture ({}) to be in correct layout (VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) before {}. Was in ({})",
			        destinationObject._debugName,
			        operation,
			        vkstring::VulkanImageLayoutToString(destinationObject.getImageLayout(baseSubresource))
			);
			this->cmdTransitionLayout(destinationHandle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}
	}
	void CommandBuffer::cmdBlitImageToSwapchain(TextureHandle source) {
		DRY_RETURN()
		const TextureHandle destination = this->_ctx->getCurrentSwapchainTexHandle();
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		auto& sourceTex = _ctx->view(source);
		auto& destinationTex = _ctx->view(destination);
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
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		auto& sourceTex = _ctx->view(source);
		auto& destinationTex = _ctx->view(destination);
#ifdef DEBUG
		CheckTextureRenderingUsage(sourceTex, destinationTex, "copy");
#endif
		CheckImageLayoutAuto(source, destination, "copying");
		// needs to be same so doesnt matter if we take source or destination size
		cmdCopyImageImpl(source, destination, _ctx->view(source).getExtentAs2D());
	}

	void CommandBuffer::cmdBlitImage(TextureHandle source, TextureHandle destination) {
		DRY_RETURN()
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

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
		ASSERT_MSG(source.valid() && destination.valid(), "Textures must be valid handles!");
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		auto& sourceTex = _ctx->view(source);
		auto& destinationTex = _ctx->view(destination);
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
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdPushConstants()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		if (!_currentPipelineInfo) {
			LOG_SYSTEM(LogType::Warning, "No pipeline currently bound, will not perform push constants!");
			return;
		}

		ASSERT_MSG(size % 4 == 0, "Push constant size needs to be a multiple of 4. Is size {}", size);
		if (const VkPhysicalDeviceLimits& limits = _ctx->getPhysicalDeviceProperties10().limits; size + offset > limits.maxPushConstantsSize) {
			LOG_SYSTEM(LogType::Error, "Push constants size exceeded %u (max %u bytes)", size + offset, limits.maxPushConstantsSize);
		}

		VkShaderStageFlags stages = 0;
		uint32_t rangeEnd = 0;
		for (const VkPushConstantRange& range: _currentPipelineInfo->core.signature.pushes) {
			if (offset < range.offset + range.size && range.offset < offset + size) {
				stages |= range.stageFlags;
				rangeEnd = std::max(rangeEnd, range.offset + range.size);
			}
		}
		size = std::min(size, rangeEnd - offset);
		if (!stages) {
			LOG_SYSTEM(LogType::Warning, "Pipeline '{}' has no push constant range overlapping [{}, {}), skipping push constants!", _currentPipelineInfo->debugName, offset, offset + size);
			return;
		}
#ifdef DEBUG
		MaybeWarnPushConstantSizeMismatch(_currentPipelineInfo->core, size, _currentPipelineInfo->debugName);
#endif
		vkCmdPushConstants(_wrapper->_cmdBuf, _currentPipelineInfo->core._vkPipelineLayout, static_cast<VkShaderStageFlagBits>(stages), offset, size, data);
	}

	void CommandBuffer::cmdBindPipelineImpl(const PipelineCoreData* common, VkPipelineBindPoint bindPoint) {
		// only bind pipeline if its not currently bound
		// what we should be actually doing anyways
		_lastBoundvkPipeline = common->_vkPipeline;
		{
			vkCmdBindPipeline(_wrapper->_cmdBuf, bindPoint, common->_vkPipeline);
		}
		if (common->_vkBindlessDescriptorSet == VK_NULL_HANDLE)
			return;
		{
			vkCmdBindDescriptorSets(_wrapper->_cmdBuf, bindPoint, common->_vkPipelineLayout, 0, 1, &common->_vkBindlessDescriptorSet, 0, nullptr);
		}
	}

	void CommandBuffer::cmdBindGraphicsPipeline(GraphicsPipelineHandle handle) {
		ASSERT(this->_ctx);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded render pipeline was invalid/empty!");
			return;
		}
		AllocatedGraphicsPipeline* pipeline = _ctx->_graphicsPipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		// the dry run hits this first so pipelines are built at graph compile, not mid frame
		if (pipeline->_shared.core._vkPipeline == VK_NULL_HANDLE)
			_ctx->resolveGraphicsPipelineImpl(*pipeline, _viewMask);
#ifdef DEBUG
		if (_activePass) {
			bool matches = pipeline->_builtViewMask == _viewMask && pipeline->_builtColorFormats.size() == _activePass->colorAttachments.size() &&
			               pipeline->_builtDepthFormat == (_activePass->depthAttachment.has_value() ? _activePass->depthAttachment->imageFormat : VK_FORMAT_UNDEFINED);
			for (size_t i = 0; matches && i < _activePass->colorAttachments.size(); ++i)
				matches = pipeline->_builtColorFormats[i] == _activePass->colorAttachments[i].imageFormat;
			ASSERT_MSG(matches, "Graphics pipeline '{}' was built for another pass's attachment formats or view mask and cannot draw in '{}', create one pipeline per attachment layout.", pipeline->getDebugName(), _activePass->name);
		}
#endif
		if (_isDryRun)
			return;
		this->_currentPipelineHandle = handle;
		this->_currentPipelineInfo = &pipeline->_shared;

		// alias it
		const SharedPipelineInfo* info = this->_currentPipelineInfo;
		CHECK_PIPELINE_REBIND(&info->core, _lastBoundvkPipeline, info->debugName);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		this->cmdBindPipelineImpl(&info->core, VK_PIPELINE_BIND_POINT_GRAPHICS);
	}

	void CommandBuffer::cmdBindComputePipeline(ComputePipelineHandle handle) {
		ASSERT(this->_ctx);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded compute pipeline was invalid/empty!");
			return;
		}
		AllocatedComputePipeline* pipeline = _ctx->_computePipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (pipeline->_shared.core._vkPipeline == VK_NULL_HANDLE)
			_ctx->resolveComputePipelineImpl(*pipeline);
		if (_isDryRun)
			return;
		this->_currentPipelineHandle = handle;
		this->_currentPipelineInfo = &pipeline->_shared;
		const SharedPipelineInfo* info = this->_currentPipelineInfo;
		CHECK_PIPELINE_REBIND(&info->core, _lastBoundvkPipeline, info->debugName);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Compute);
		this->cmdBindPipelineImpl(&info->core, VK_PIPELINE_BIND_POINT_COMPUTE);
	}
	void CommandBuffer::cmdBindRayTracingPipeline(RayTracingPipelineHandle handle) {
		ASSERT(this->_ctx);
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Binded raytracing pipeline was invalid/empty!");
			return;
		}
		AllocatedRayTracingPipeline* pipeline = _ctx->_rayTracingPipelinePool.get(handle);
		this->_ctx->checkAndUpdateBindlessDescriptorSetImpl();
		if (pipeline->_shared.core._vkPipeline == VK_NULL_HANDLE)
			_ctx->resolveRayTracingPipelineImpl(*pipeline);
		if (_isDryRun)
			return;
		this->_currentPipelineHandle = handle;
		this->_currentPipelineInfo = &pipeline->_shared;
		const SharedPipelineInfo* info = this->_currentPipelineInfo;
		CHECK_PIPELINE_REBIND(&info->core, _lastBoundvkPipeline, info->debugName);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::RayTracing);
		this->cmdBindPipelineImpl(&info->core, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
	}

	void CommandBuffer::cmdUpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		ASSERT_MSG(!_isRendering, "You cannot update a buffer while rendering! Please move this command either before or after rendering.");
		ASSERT_MSG(handle.valid(), "cmdUpdateBuffer called with an invalid BufferHandle.");
		ASSERT_MSG(size > 0 && size <= 65536, "cmdUpdateBuffer requires 0 < size <= 65'536 (64KB).");
		ASSERT_MSG(size % 4 == 0, "cmdUpdateBuffer size must be a multiple of 4 bytes.");
		ASSERT_MSG(offset % 4 == 0, "cmdUpdateBuffer offset must be a multiple of 4 bytes.");
		AllocatedBuffer* buf = _ctx->_bufferPool.get(handle);
		ASSERT_MSG(buf, "cmdUpdateBuffer: handle does not resolve to a live buffer.");
		ASSERT_MSG(offset + size <= buf->_bufferSize, "cmdUpdateBuffer: offset + size exceeds buffer '{}' size ({} > {}).", buf->_debugName, offset + size, buf->_bufferSize);
		ASSERT_MSG(buf->_vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, "cmdUpdateBuffer: buffer '{}' was not created with TRANSFER_DST usage.", buf->_debugName);
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "You are updating buffer '{}' with empty data.", buf->_debugName);
			return;
		}

		const vkutil::BufferSyncFeatures features = {
		    .accelerationStructure = _ctx->isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
		    .rayTracingPipeline = _ctx->isExtensionEnabled(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME),
		};
		const vkutil::UpdateBufferBarriers barriers = vkutil::GetUpdateBufferBarriers(buf->_vkUsageFlags, features);
		bufferBarrierImpl(*buf, barriers.before.src, barriers.before.dst);

		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdUpdateBuffer()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdUpdateBuffer(_wrapper->_cmdBuf, buf->_vkBuffer, offset, size, data);

		bufferBarrierImpl(*buf, barriers.after.src, barriers.after.dst);
	}
	void CommandBuffer::cmdCopyImageToBuffer(TextureHandle source, BufferHandle destination, const VkBufferImageCopy& region) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);

		auto& sourceTex = _ctx->view(source);
		const VkImageSubresourceLayers& layers = region.imageSubresource;
		const SubresourceRange range = {layers.mipLevel, 1, layers.baseArrayLayer, layers.layerCount};
		const VkImageLayout currentLayout = sourceTex.getImageLayout(range);
		if (currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && currentLayout != VK_IMAGE_LAYOUT_GENERAL) {
			cmdTransitionLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, {layers.aspectMask, layers.mipLevel, 1, layers.baseArrayLayer, layers.layerCount});
		}
		const VkImageLayout properLayout = sourceTex.getImageLayout(range);
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdCopyImageToBuffer()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdCopyImageToBuffer(_wrapper->_cmdBuf, sourceTex._vkImage, properLayout, _ctx->view(destination)._vkBuffer, 1, &region);
	}
	void CommandBuffer::cmdBindDepthState(const DepthState& state) {
		DRY_RETURN()
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		// https://github.com/corporateshark/lightweightvk/blob/master/lvk/vulkan/VulkanClasses.cpp#L2458
		const VkCompareOp op = toVulkan(state.compareOp);
		{
			vkCmdSetDepthWriteEnable(_wrapper->_cmdBuf, state.isDepthWriteEnabled ? VK_TRUE : VK_FALSE);
		}
		{
			vkCmdSetDepthTestEnable(_wrapper->_cmdBuf, (op != VK_COMPARE_OP_ALWAYS || state.isDepthWriteEnabled) ? VK_TRUE : VK_FALSE);
		}
		{
			vkCmdSetDepthCompareOp(_wrapper->_cmdBuf, op);
		}
	}
	void CommandBuffer::cmdSetDepthBiasEnable(bool enable) {
		DRY_RETURN();
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, enable ? VK_TRUE : VK_FALSE);
	}
	void CommandBuffer::cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp) {
		DRY_RETURN();
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_COMMAND);
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
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
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdDrawImgui()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		CHECK_SHOULD_BE_RENDERING();
		CHECK_PASS_OPERATION_MISMATCH(PassDesc::Type::Graphics);
#ifdef DEBUG
		for (const auto& color_attachment: this->_activePass->colorAttachments) {
			ASSERT_MSG(color_attachment.resolveImageView == VK_NULL_HANDLE, "Rendering of ImGui cannot be done inside a multisampled texture!");
			ASSERT_MSG(
			        color_attachment.imageFormat == this->_ctx->_imguiPlugin.getFormat(),
			        "ImGui is drawing to a format that it was not given! Please pass the VkFormat of the texture which ImGui is drawing to when calling 'withImGuiPlugin()'!"
			);
		}
#endif
		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _wrapper->_cmdBuf);
		// ImGui binds its own pipeline, so the rebind skip must not trust the cached one
		_lastBoundvkPipeline = VK_NULL_HANDLE;
	}
#endif

	// ALL INTERNALLY CALLED COMMAND BUFFER FUNCTIONS
	void CommandBuffer::cmdBeginRenderingImpl(uint32_t layerCount, uint32_t viewMask) {
		ASSERT_MSG(!_isRendering, "Command Buffer is already rendering!");
		const bool hasDepth = _activePass->depthAttachment.has_value();
		const VkRect2D renderArea = _activePass->renderArea;
		std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> vk_color_attachment_infos{};
		ASSERT(vk_color_attachment_infos.size() >= _activePass->colorAttachments.size());
		for (int i = 0; i < _activePass->colorAttachments.size(); i++) {
			const auto& attachmentInfo = _activePass->colorAttachments[i];
			vk_color_attachment_infos[i] = attachmentInfo.getAsVkRenderingAttachmentInfo();
		}
		VkRenderingAttachmentInfo vk_depth_attachment_info{};
		if (hasDepth) {
			vk_depth_attachment_info = _activePass->depthAttachment->getAsVkRenderingAttachmentInfo();
		}
		const VkRenderingInfo info = {
		    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		    .pNext = nullptr,
		    .flags = 0,
		    .renderArea = renderArea,
		    .layerCount = layerCount,
		    .viewMask = viewMask,
		    .colorAttachmentCount = static_cast<uint32_t>(_activePass->colorAttachments.size()),
		    .pColorAttachments = vk_color_attachment_infos.data(),
		    .pDepthAttachment = hasDepth ? &vk_depth_attachment_info : nullptr,
		    .pStencilAttachment = nullptr
		};
		_ctx->checkAndUpdateBindlessDescriptorSetImpl();

		vkCmdBeginRendering(_wrapper->_cmdBuf, &info);
		_isRendering = true;
		resetMemberStateImpl();
	}

	void CommandBuffer::resetMemberStateImpl() {
		_currentPipelineInfo = nullptr;
		_currentPipelineHandle = {};
		_hasDispatchedInPass = false;
		if (!_isRendering)
			return;
		const VkExtent2D extent = _activePass->renderArea.extent;
		cmdSetViewportImpl(extent);
		cmdSetScissorImpl(extent);
		cmdBindDepthState({});
		vkCmdSetDepthBiasEnable(_wrapper->_cmdBuf, VK_FALSE);
	}

	void CommandBuffer::cmdEndRenderingImpl() {
		ASSERT_MSG(_isRendering, "Command buffer isnt even rendering!");
		vkCmdEndRendering(_wrapper->_cmdBuf);
		_isRendering = false;
	}
	void CommandBuffer::bufferBarrierImpl(const AllocatedBuffer& buf, vkutil::StageAccess src, vkutil::StageAccess dst) {
		const VkBufferMemoryBarrier2 barrier = {
		    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		    .srcStageMask = src.stage,
		    .srcAccessMask = src.access,
		    .dstStageMask = dst.stage,
		    .dstAccessMask = dst.access,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .buffer = buf._vkBuffer,
		    .offset = 0,
		    .size = VK_WHOLE_SIZE,
		};
		const VkDependencyInfo depInfo = {
		    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		    .bufferMemoryBarrierCount = 1,
		    .pBufferMemoryBarriers = &barrier,
		};
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdPipelineBarrier2()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdPipelineBarrier2(_wrapper->_cmdBuf, &depInfo);
	}
	void CommandBuffer::cmdCopyImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D size) {
		auto& sourceTex = _ctx->view(source);
		auto& destinationTex = _ctx->view(destination);
		ASSERT_MSG((sourceTex._vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) > 0, "Source image must have VK_IMAGE_USAGE_TRANSFER_SRC_BIT!");
		ASSERT_MSG((destinationTex._vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) > 0, "Destination image must have VK_IMAGE_USAGE_TRANSFER_DST_BIT!");
		ASSERT_MSG(sourceTex._numLayers == destinationTex._numLayers, "The images being copied must have the same number of layers!");
		ASSERT_MSG(
		        sourceTex._vkFormat == destinationTex._vkFormat,
		        "The images being copied must be of the same VkFormat (source is '{}' while destination is '{}')!",
		        vkstring::VulkanFormatToString(sourceTex.getFormat()),
		        vkstring::VulkanFormatToString(destinationTex.getFormat())
		);
		ASSERT_MSG(sourceTex._vkExtent.width == destinationTex._vkExtent.width, "The images being copied must be of the same width!");
		ASSERT_MSG(sourceTex._vkExtent.height == destinationTex._vkExtent.height, "The images being copied must be of the same height!");

		VkImageCopy2 copyRegion = {.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2, .pNext = nullptr};

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

		VkCopyImageInfo2 copyinfo = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, .pNext = nullptr};
		copyinfo.dstImage = destinationTex._vkImage;
		copyinfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		copyinfo.srcImage = sourceTex._vkImage;
		copyinfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyinfo.regionCount = 1;
		copyinfo.pRegions = &copyRegion;

		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdCopyImage2()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdCopyImage2(_wrapper->_cmdBuf, &copyinfo);
	}
	void CommandBuffer::cmdBlitImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize) {
		VkImageBlit2 blitRegion = {.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr};

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

		VkBlitImageInfo2 blitInfo = {.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr};
		blitInfo.srcImage = _ctx->view(source)._vkImage;
		blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		blitInfo.dstImage = _ctx->view(destination)._vkImage;
		blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		blitInfo.filter = VK_FILTER_NEAREST; // todo allow customization
		blitInfo.regionCount = 1;
		blitInfo.pRegions = &blitRegion;

		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdBlitImage2()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdBlitImage2(_wrapper->_cmdBuf, &blitInfo);
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
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdSetViewport()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdSetViewport(_wrapper->_cmdBuf, 0, 1, &viewport);
	}
	void CommandBuffer::cmdSetScissorImpl(VkExtent2D extent2D) {
		VkRect2D scissor = {};
		scissor.offset.x = 0; // default 0 for both
		scissor.offset.y = 0;
		scissor.extent = extent2D;
		MYTH_PROFILER_GPU_COMMAND_ZONE("cmdSetScissor()", _wrapper->_cmdBuf, MYTH_PROFILER_COLOR_COMMAND);
		vkCmdSetScissor(_wrapper->_cmdBuf, 0, 1, &scissor);
	}

} // namespace mythril
