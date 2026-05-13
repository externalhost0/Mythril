//
// Created by Hayden Rivas on 10/11/25.
//

#include "mythril/CTX.h"
#include "CommandBuffer.h"
#include "mythril/vkenums.h"

#include "mythril/RenderGraphBuilder.h"
#include "RenderGraphInternal.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {
	// Out-of-line definitions required so that RenderGraph's std::vector / std::unordered_map members
	// see complete types only here — public RenderGraphBuilder.h keeps them forward-declared.
	RenderGraph::RenderGraph() = default;
	RenderGraph::~RenderGraph() = default;
	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	BasePassBuilder::BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type)
		: _rGraph(rGraph), _passSource(pName, type) {
		ASSERT(!_passSource.name.empty());
	}

#ifdef DEBUG
	GraphicsPassBuilder::~GraphicsPassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "GraphicsPass '{}' has no execution callback!", this->base._passSource.name);
	}
	ComputePassBuilder::~ComputePassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "ComputePass '{}' has no execution callback!", this->base._passSource.name);
	}
#endif

	IntermediateBuilder& IntermediateBuilder::blit(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, Layout::TRANSFER_SRC);
		add(base._passSource, dst, Layout::TRANSFER_DST);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture->isSwapchainImage()) {
			base._passSource.executeCallback = [oldCallback, srcHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdBlitImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdBlitImage(srcHandle, dstHandle);
			};
		}
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::copy(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, Layout::TRANSFER_SRC);
		add(base._passSource, dst, Layout::TRANSFER_DST);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture->isSwapchainImage()) {
			base._passSource.executeCallback = [oldCallback, srcHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdCopyImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdCopyImage(srcHandle, dstHandle);
			};
		}
		return *this;
	}

	// todo: i have no clue how this just works immediately besides the fact we dont actually touch the base image + restore its original layout
	IntermediateBuilder& IntermediateBuilder::generateMipmaps(const Texture& texture) {
		TextureHandle handle = texture.handle();
		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			cmd.cmdGenerateMipmap(handle);
		};
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::dependency(Buffer& buffer, BufferAccess access) {
		add(base._passSource, buffer, access);
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::update(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		BufferHandle handle = buffer.handle();
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle, dataCb = std::move(dataCb), dstOffset](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			if (cmd.isDrying())
				return;

			const UploadData data = dataCb();
			ASSERT_MSG(data.data, "IntermediateBuilder::update called with null data.");
			ASSERT_MSG(data.size > 0 && data.size <= 65536, "IntermediateBuilder::update requires 0 < size <= 65'536 (64KB).");
			ASSERT_MSG(data.size % 4 == 0, "IntermediateBuilder::update size must be a multiple of 4 bytes.");
			ASSERT_MSG(dstOffset % 4 == 0, "IntermediateBuilder::update offset must be a multiple of 4 bytes.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_MSG(dstOffset + data.size <= buf._bufferSize, "IntermediateBuilder::update: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_MSG(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, "IntermediateBuilder::update: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);

			vkCmdUpdateBuffer(cmd._wrapper->_cmdBuf, buf._vkBuffer, dstOffset, data.size, data.data);
		};
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		BufferHandle handle = buffer.handle();
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle, dataCb = std::move(dataCb), dstOffset](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			if (cmd.isDrying())
				return;

			const UploadData data = dataCb();
			ASSERT_MSG(data.data, "IntermediateBuilder::upload called with null data.");
			ASSERT_MSG(data.size > 0, "IntermediateBuilder::upload size must be greater than 0.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_MSG(dstOffset + data.size <= buf._bufferSize, "IntermediateBuilder::upload: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_MSG(
			        buf.isMapped() || (buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), "IntermediateBuilder::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName
			);
			if (buf.isMapped()) {
				buf.bufferSubData(*cmd._ctx, dstOffset, data.size, data.data);
				return;
			}

			auto copies = cmd._ctx->_staging->stageBufferCopy(buf, data.data, data.size, dstOffset);
			cmd._ctx->_staging->recordBufferCopies(cmd._wrapper->_cmdBuf, copies);
		};
		return *this;
	}
	void IntermediateBuilder::finish() {
		this->base._rGraph._passDescriptions.push_back(base._passSource);
		this->base._rGraph._hasCompiled = false;
	}

	void BasePassBuilder::setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback) {
		_passSource.executeCallback = callback;
		_rGraph._passDescriptions.push_back(_passSource);
		_rGraph._hasCompiled = false;
	}

	static constexpr bool IsAttachmentImageLayout(VkImageLayout layout) {
		switch (layout) {
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
				return true;
			default:
				return false;
		}
	}
	static constexpr VkImageSubresourceRange MakeVkRange(VkFormat fmt, SubresourceRange range) {
		return {vkutil::AspectMaskFromFormat(fmt), range.baseMip, range.numMips, range.baseLayer, range.numLayers};
	}
	static SubresourceRange ResolveSubresourceRange(const TextureDesc& texDesc, const AllocatedTexture& texture, std::string_view passName) {
		const uint32_t baseMip = texDesc.baseLevel.value_or(0);
		const uint32_t numMips = texDesc.numLevels.value_or(texDesc.baseLevel.has_value() ? 1 : texture.getNumMips());
		const uint32_t baseLayer = texDesc.baseLayer.value_or(0);
		const uint32_t numLayers = texDesc.numLayers.value_or(texDesc.baseLayer.has_value() ? 1 : texture.getNumLayers());

		ASSERT_MSG(baseMip < texture.getNumMips(), "Pass '{}': Texture '{}' base mip {} is outside the texture's {} mip levels", passName, texture.getDebugName(), baseMip, texture.getNumMips());
		ASSERT_MSG(
		        numMips > 0 && baseMip + numMips <= texture.getNumMips(),
		        "Pass '{}': Texture '{}' mip range [{}, {}) is outside the texture's {} mip levels",
		        passName,
		        texture.getDebugName(),
		        baseMip,
		        baseMip + numMips,
		        texture.getNumMips()
		);
		ASSERT_MSG(baseLayer < texture.getNumLayers(), "Pass '{}': Texture '{}' base layer {} is outside the texture's {} layers", passName, texture.getDebugName(), baseLayer, texture.getNumLayers());
		ASSERT_MSG(
		        numLayers > 0 && baseLayer + numLayers <= texture.getNumLayers(),
		        "Pass '{}': Texture '{}' layer range [{}, {}) is outside the texture's {} layers",
		        passName,
		        texture.getDebugName(),
		        baseLayer,
		        baseLayer + numLayers,
		        texture.getNumLayers()
		);

		return {baseMip, numMips, baseLayer, numLayers};
	}

	static VkImageView ResolveImageView(const CTX& rCtx, const TextureDesc& texDesc, const AllocatedTexture& allocatedTexture, std::string_view passName) {
		const bool needsCustomView = texDesc.baseLayer.has_value() || texDesc.baseLevel.has_value() || texDesc.numLayers.has_value() || texDesc.numLevels.has_value();
		if (!needsCustomView) {
			return allocatedTexture.getImageView();
		}

		const SubresourceRange range = ResolveSubresourceRange(texDesc, allocatedTexture, passName);
		auto& mutableTexture = const_cast<Texture&>(texDesc.texture);
		VkImageViewType viewType = allocatedTexture.getViewType();
		if (range.numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
			viewType = VK_IMAGE_VIEW_TYPE_2D;
		}
		Texture::ViewKey viewKey = mutableTexture.createView({
		    .type = viewType,
		    .mipLevel = range.baseMip,
		    .numMipLevels = range.numMips,
		    .layer = range.baseLayer,
		    .numLayers = range.numLayers,
		});

		TextureHandle viewHandle = mutableTexture.handle(viewKey);
		const AllocatedTexture& viewTexture = rCtx.view(viewHandle);
		return viewTexture.getImageView();
	}

	static constexpr bool NeedsBarrier(const SubresourceState& currentState, VkImageLayout desiredLayout, const vkutil::StageAccess& desiredStageAccess) {
		if (currentState.layout != desiredLayout)
			return true;

		constexpr VkAccessFlags2 write_flags2 =
		        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

		bool currentStateWrites = (currentState.mask.access & write_flags2) > 0;
		// if (currentStateWrites || IsAttachmentImageLayout(desiredLayout))
		// 	return true;
		if (currentStateWrites && desiredStageAccess.stage != currentState.mask.stage)
			return true;

		if (currentState.mask.stage != desiredStageAccess.stage) {
			return true;
		}
		return false;
	}
	static constexpr bool IsBufferWriteAccess(VkAccessFlags2 access) {
		constexpr VkAccessFlags2 writeFlags = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
		return (access & writeFlags) != 0;
	}
	static constexpr bool NeedsBufferBarrier(const vkutil::StageAccess& currentState, const vkutil::StageAccess& desiredStageAccess) {
		if (currentState.stage == 0 && currentState.access == 0)
			return false;

		const bool currentWrites = IsBufferWriteAccess(currentState.access);
		const bool desiredWrites = IsBufferWriteAccess(desiredStageAccess.access);
		if (!currentWrites && !desiredWrites)
			return false;

		return true;
	}
	static vkutil::StageAccess GetBufferStageAccess(BufferAccess access, PassDesc::Type passType) {
		const auto shaderStages = [passType] {
			switch (passType) {
				case PassDesc::Type::Graphics:
					return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
				case PassDesc::Type::Compute:
					return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				case PassDesc::Type::Intermediate:
				case PassDesc::Type::Presentation:
					return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			}
			return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		}();

		switch (access) {
			case BufferAccess::ShaderRead:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
			case BufferAccess::ShaderWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
			case BufferAccess::ShaderReadWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
			case BufferAccess::IndexRead:
				return {.stage = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, .access = VK_ACCESS_2_INDEX_READ_BIT};
			case BufferAccess::IndirectRead:
				return {.stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, .access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
			case BufferAccess::TransferRead:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT};
			case BufferAccess::TransferWrite:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT};
		}
		ASSERT_MSG(false, "Unsupported BufferAccess value.");
	}

	void RenderGraph::processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass, std::string_view passName) {
		const AllocatedTexture& texture = texDesc.texture.view();
		const bool isSwapchain = texture.isSwapchainImage();
		const SubresourceRange range = ResolveSubresourceRange(texDesc, texture, passName);
		const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(desiredLayout);
		outPass.imageBarriers.push_back({.handle = texDesc.texture.handle(), .range = range, .dstLayout = desiredLayout, .dstMask = dstMask, .isSwapchain = isSwapchain});
	}

	void RenderGraph::processPassResources(const PassDesc& passDesc, CompiledPass& outPass) {
		// we expect to use a max of this
		outPass.imageBarriers.reserve(passDesc.dependencyOperations.size() + passDesc.attachmentOperations.size());
		outPass.bufferBarriers.reserve(passDesc.bufferDependencyOperations.size());
		for (const DependencyDesc& dependency_desc: passDesc.dependencyOperations) {
			const VkImageLayout layout = [](const Layout simpleLayout) {
				switch (simpleLayout) {
					case Layout::GENERAL:
						return VK_IMAGE_LAYOUT_GENERAL;
					case Layout::READ:
						return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					case Layout::TRANSFER_SRC:
						return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					case Layout::TRANSFER_DST:
						return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					case Layout::PRESENT:
						return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
					default:
						assert(false);
				}
			}(dependency_desc.desiredLayout);
			processResourceAccess(dependency_desc.texDesc, layout, outPass, passDesc.name);
		}
		for (const AttachmentDesc& attachment_desc: passDesc.attachmentOperations) {
			const AllocatedTexture& texture = attachment_desc.texDesc.texture.view();
			const VkImageLayout layout = texture.isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			processResourceAccess(attachment_desc.texDesc, layout, outPass, passDesc.name);

			if (attachment_desc.resolveTexDesc.has_value()) {
				processResourceAccess(attachment_desc.resolveTexDesc.value(), layout, outPass, passDesc.name);
			}
		}
		for (const BufferDependencyDesc& dependency_desc: passDesc.bufferDependencyOperations) {
			outPass.bufferBarriers.push_back({.handle = dependency_desc.buffer.handle(), .dstMask = GetBufferStageAccess(dependency_desc.access, passDesc.type)});
		}
	}

	void RenderGraph::processAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) {
		if (pass_desc.attachmentOperations.empty())
			return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc: pass_desc.attachmentOperations) {
			const TextureDesc& texDesc = attachment_desc.texDesc;
			const AllocatedTexture& allocatedTexture = attachment_desc.texDesc.texture.view();

			const bool isDepthAttachment = vkutil::IsFormatDepth(allocatedTexture.getFormat());
			if (isDepthAttachment) {
				ASSERT_MSG(!hasDepthAttachment, "Pass '{}': Multiple depth attachments not allowed (found '{}' to be a second depth attachment)", outPass.name, allocatedTexture.getDebugName());
				hasDepthAttachment = true;
			}
			VkImageView imageView = ResolveImageView(rCtx, texDesc, allocatedTexture, pass_desc.name);

			const ClearValue& clear_value = attachment_desc.clearValue;
			AttachmentInfo attachment_info = {
			    .imageFormat = allocatedTexture.getFormat(),
			    .imageView = imageView,
			    .imageLayout = isDepthAttachment ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    // these fields are "resolved" in the next steps, haha
			    .resolveImageView = VK_NULL_HANDLE,
			    .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,

			    .loadOp = toVulkan(attachment_desc.loadOp),
			    .storeOp = toVulkan(attachment_desc.storeOp),
			    .clearValue = isDepthAttachment ? clear_value.getDepthStencilValue() : clear_value.getColorValue(),
			};
			if (attachment_desc.resolveTexDesc.has_value()) {
				const TextureDesc& resolve_desc = attachment_desc.resolveTexDesc.value();
				const AllocatedTexture resolve_texture = resolve_desc.texture.view();
				ASSERT_MSG(
				        resolve_texture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
				        "Pass '{}': Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
				        pass_desc.name,
				        resolve_texture.getDebugName()
				);
				ASSERT_MSG(
				        allocatedTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT, "Pass '{}': Resolve operation on non-multisampled texture '{}'!", pass_desc.name, allocatedTexture.getDebugName()
				);
				ASSERT_MSG(
				        resolve_texture.getFormat() == allocatedTexture.getFormat(),
				        "Pass '{}': Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
				        pass_desc.name,
				        allocatedTexture.getDebugName(),
				        vkstring::VulkanFormatToString(allocatedTexture.getFormat()),
				        resolve_texture.getDebugName(),
				        vkstring::VulkanFormatToString(resolve_texture.getFormat())
				);
				if (attachment_desc.storeOp == StoreOp::STORE)
					LOG_SYSTEM_NOSOURCE(
					        LogType::Suggestion,
					        "Pass '{}': Attachment of texture '{}' has StoreOp::STORE and has a resolve attachment, can be replaced with StoreOp::NO_CARE.",
					        pass_desc.name,
					        allocatedTexture.getDebugName(),
					        resolve_texture.getDebugName()
					);

				// set the fields we left empty previously
				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveImageView = ResolveImageView(rCtx, resolve_desc, resolve_texture, pass_desc.name);
			}
			if (allocatedTexture.isSwapchainImage()) {
				attachment_info.isSwapchainImage = true;
				const auto& swapchain = rCtx._swapchain;
				const uint32_t numImages = swapchain->getNumOfSwapchainImages();
				for (uint32_t i = 0; i < numImages; i++) {
					const TextureHandle handle = swapchain->getSwapchainTextureHandle(i);
					attachment_info.swapchainImageViews[i] = rCtx.view(handle).getImageView();
				}
			}

			// submit AttachmentInfo
			if (isDepthAttachment)
				outPass.depthAttachment.emplace(attachment_info);
			else
				outPass.colorAttachments.emplace_back(attachment_info);

			// calculate the necessary dimensions of the vkCmdBeginRendering call
			// thats all the below code does
			const Dimensions& baseDims = allocatedTexture.getDimensions();
			const uint32_t mipLevel = texDesc.baseLevel.value_or(0);
			const uint32_t width = std::max(1u, baseDims.width >> mipLevel);
			const uint32_t height = std::max(1u, baseDims.height >> mipLevel);

			if ((max_width != width || max_height != height) && (max_width != 0 || max_height != 0))
				LOG_SYSTEM(LogType::Warning, "Pass '{}': You have attachments of different dimensions, this is allowed but experimental.", pass_desc.name);
			max_width = std::max(max_width, width);
			max_height = std::max(max_height, height);
		}
		outPass.renderArea = {{0, 0}, {max_width, max_height}};
	}

	void RenderGraph::performDryRun(CTX& rCtx) {
		// preserve any in-flight command buffer (e.g. when compile() is triggered from inside
		// execute() after acquireCommand() has installed the real cmd buffer).
		const CommandBuffer saved = rCtx._currentCommandBuffer;
		for (const CompiledPass& pass: _compiledPasses) {
			// by default CommandBuffer will have _isDryRun = true
			CommandBuffer dryCmd;
			dryCmd._ctx = &rCtx;
			dryCmd._activePass = pass;
			rCtx._currentCommandBuffer = dryCmd;
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, something went horribly wrong!", pass.name);
			pass.executeCallback(dryCmd);
		}
		rCtx._currentCommandBuffer = saved;
	}

	void RenderGraph::compile(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		_compiledPasses.clear();
		_resourceTrackers.clear();
		_bufferTrackers.clear();
		// compile works in this order
		_compiledPasses.reserve(_passDescriptions.size());
		for (uint32_t passIndex = 0; passIndex < _passDescriptions.size(); passIndex++) {
			const PassDesc& pass_desc = _passDescriptions[passIndex];
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.passIndex = passIndex;
			compiled_pass.type = pass_desc.type;
			compiled_pass.executeCallback = pass_desc.executeCallback;

			// do not worry about the return value,
			// every type of process should run for every pass
			processPassResources(pass_desc, compiled_pass);
			processAttachments(rCtx, pass_desc, compiled_pass);

			_compiledPasses.push_back(std::move(compiled_pass));
		}
		performDryRun(rCtx);
		_hasCompiled = true;
		_compiledEpoch = rCtx._resourceEpoch;
	}

	void RenderGraph::trackWindowSized(Texture& texture) {
		trackWindowSized(texture, [](const Dimensions& dims) { return dims; });
	}

	void RenderGraph::trackWindowSized(Texture& texture, std::function<Dimensions(Dimensions)> scaleFn) {
		ASSERT_MSG(texture.valid(), "RenderGraph::trackWindowSized called with an invalid texture.");
		_windowSizedTextures.push_back({.texture = &texture, .scaleFn = std::move(scaleFn)});
	}

	void RenderGraph::resizeTrackedWindowSized(const Dimensions& swapchainDimensions) const {
		for (const WindowSizedTexture& tracked: _windowSizedTextures) {
			ASSERT(tracked.texture);
			const Dimensions nextDimensions = tracked.scaleFn ? tracked.scaleFn(swapchainDimensions) : swapchainDimensions;
			tracked.texture->resize(nextDimensions);
		}
	}

	void RenderGraph::PerformBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass) {
		if (compiledPass.imageBarriers.empty() && compiledPass.bufferBarriers.empty())
			return;
		ASSERT(cmd._wrapper->_cmdBuf);

		std::vector<VkImageMemoryBarrier2> vkImageBarriers;
		vkImageBarriers.reserve(compiledPass.imageBarriers.size());
		std::vector<VkBufferMemoryBarrier2> vkBufferBarriers;
		vkBufferBarriers.reserve(compiledPass.bufferBarriers.size());
		// we have to update the image associated if its a swapchain image
		// as it changes every frame
		for (const CompiledImageBarrier& req: compiledPass.imageBarriers) {
			const TextureHandle activeHandle = req.isSwapchain ? cmd._ctx->getCurrentSwapchainTexHandle() : req.handle;
			const AllocatedTexture& activeTexture = cmd._ctx->view(activeHandle);
			if (!_resourceTrackers.contains(activeHandle)) {
				_resourceTrackers.emplace(activeHandle, TextureStateTracker{activeTexture.getNumMips(), activeTexture.getNumLayers()});
			}
			TextureStateTracker& tracker = _resourceTrackers.at(activeHandle);
			const std::vector<TextureStateTracker::SubresourceEntry> currentStates = tracker.getOverlappingStates(req.range);
			for (const auto& current: currentStates) {
				if (!NeedsBarrier(current.state, req.dstLayout, req.dstMask))
					continue;
				vkImageBarriers.push_back(
				        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				         .srcStageMask = current.state.mask.stage,
				         .srcAccessMask = current.state.mask.access,
				         .dstStageMask = req.dstMask.stage,
				         .dstAccessMask = req.dstMask.access,
				         .oldLayout = current.state.layout,
				         .newLayout = req.dstLayout,
				         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .image = activeTexture.getImage(),
				         .subresourceRange = MakeVkRange(activeTexture.getFormat(), current.range)}
				);
			}
			tracker.setState(req.range, {req.dstLayout, req.dstMask});
			cmd._ctx->access(activeHandle)._vkCurrentImageLayout = req.dstLayout;
		}
		for (const CompiledBufferBarrier& req: compiledPass.bufferBarriers) {
			const AllocatedBuffer& buffer = cmd._ctx->view(req.handle);
			const vkutil::StageAccess currentState = _bufferTrackers.contains(req.handle) ? _bufferTrackers.at(req.handle) : vkutil::StageAccess{};
			if (NeedsBufferBarrier(currentState, req.dstMask)) {
				vkBufferBarriers.push_back(
				        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				         .srcStageMask = currentState.stage,
				         .srcAccessMask = currentState.access,
				         .dstStageMask = req.dstMask.stage,
				         .dstAccessMask = req.dstMask.access,
				         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .buffer = buffer._vkBuffer,
				         .offset = 0,
				         .size = VK_WHOLE_SIZE}
				);
			}
			_bufferTrackers[req.handle] = req.dstMask;
		}
		if (!vkImageBarriers.empty() || !vkBufferBarriers.empty()) {
			const VkDependencyInfo dependencyInfo = {
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .pNext = nullptr,
			    .bufferMemoryBarrierCount = static_cast<uint32_t>(vkBufferBarriers.size()),
			    .pBufferMemoryBarriers = vkBufferBarriers.data(),
			    .imageMemoryBarrierCount = static_cast<uint32_t>(vkImageBarriers.size()),
			    .pImageMemoryBarriers = vkImageBarriers.data()
			};
			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
		}
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		// auto-recompile if any resource topology changed since last compile (texture resize, swapchain recreate/destroy).
		// single uint64 compare on the hot path; recompile only runs when actually stale (typically window-resize frames).
		if (_compiledEpoch != cmd._ctx->_resourceEpoch) {
			compile(*cmd._ctx);
		}

		for (CompiledPass& pass: _compiledPasses) {
			// perform batched vkCmdPipelineBarrier
			PerformBarrierTransitions(cmd, pass);
			// switch color attachment imageViews for Swapchain
			if (!cmd._ctx->isHeadless()) {
				const uint32_t swapIdx = cmd._ctx->_swapchain->getCurrentImageIndex();
				for (auto& color: pass.colorAttachments) {
					if (color.isSwapchainImage)
						color.imageView = color.swapchainImageViews[swapIdx];
				}
				if (pass.depthAttachment && pass.depthAttachment->isSwapchainImage)
					pass.depthAttachment->imageView = pass.depthAttachment->swapchainImageViews[swapIdx];
			}
			// reset current states
			cmd._currentPipelineInfo = nullptr;
			cmd._currentPipelineHandle = {};
			cmd._activePass = pass;
			// we already checked if it has an execute callback so it should be guaranteed
			pass.executeCallback(cmd);
		}
		if (!cmd._ctx->isHeadless()) {
			// fixme: make this cleaner im so lazy right now
			cmd.cmdTransitionLayout(cmd._ctx->getCurrentSwapchainTexHandle(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			const TextureHandle currentSwapchainHandle = cmd._ctx->getCurrentSwapchainTexHandle();
			if (_resourceTrackers.contains(currentSwapchainHandle)) {
				TextureStateTracker& tracker = _resourceTrackers.at(currentSwapchainHandle);
				tracker.setState(tracker.wholeResourceRange(), SubresourceState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, dstMask});
			}
			cmd._ctx->access(currentSwapchainHandle)._vkCurrentImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		}
	}
} // namespace mythril
