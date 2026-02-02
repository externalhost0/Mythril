//
// Created by Hayden Rivas on 10/11/25.
//

#include "CommandBuffer.h"
#include "vkenums.h"
#include "CTX.h"

#include "mythril/RenderGraphBuilder.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {
	IntermediateBuilder& IntermediateBuilder::blit(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, Layout::TRANSFER_SRC);
		add(base._passSource, dst, Layout::TRANSFER_DST);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture->isSwapchainImage()) {
			base._passSource.executeCallback = [oldCallback, srcHandle](CommandBuffer& cmd) {
				if (oldCallback) oldCallback(cmd);
				cmd.cmdBlitImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback) oldCallback(cmd);
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
				if (oldCallback) oldCallback(cmd);
				cmd.cmdCopyImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback) oldCallback(cmd);
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
			if (oldCallback) oldCallback(cmd);
			cmd.cmdGenerateMipmap(handle);
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
			default: return false;
		}

	}
	static constexpr VkImageSubresourceRange MakeVkRange(VkFormat fmt,  SubresourceRange range) {
		return {vkutil::AspectMaskFromFormat(fmt), range.baseMip, range.numMips, range.baseLayer, range.numLayers};
	}
	static constexpr bool NeedsBarrier(const SubresourceState& currentState, VkImageLayout desiredLayout, const vkutil::StageAccess& desiredStageAccess) {
		if (currentState.layout != desiredLayout)
			return true;

		constexpr VkAccessFlags2 write_flags2 =
			VK_ACCESS_2_SHADER_WRITE_BIT |
				VK_ACCESS_2_MEMORY_WRITE_BIT |
					VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
						VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
							VK_ACCESS_2_TRANSFER_WRITE_BIT;

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

	void RenderGraph::processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass) {
		const AllocatedTexture& texture = texDesc.texture.view();
		const bool isSwapchain = texture.isSwapchainImage();
		const SubresourceRange range = {
			.baseMip = texDesc.baseLevel.value_or(0),
			.numMips = texDesc.numLevels.value_or(texture.getNumMips()),
			.baseLayer = texDesc.baseLayer.value_or(0),
			.numLayers = texDesc.numLayers.value_or(texture.getNumLayers())
		};
		const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(desiredLayout);
		outPass.imageBarriers.push_back({
			.handle = texDesc.texture.handle(),
			.range = range,
			.dstLayout = desiredLayout,
			.dstMask = dstMask,
			.isSwapchain = isSwapchain
		});
	}

	void RenderGraph::processPassResources(const PassDesc& passDesc, CompiledPass& outPass) {
		// we expect to use a max of this
		outPass.imageBarriers.reserve(passDesc.dependencyOperations.size() + passDesc.attachmentOperations.size());
		for (const DependencyDesc& dependency_desc : passDesc.dependencyOperations) {
			const VkImageLayout layout = [](const Layout simpleLayout) {
				switch (simpleLayout) {
					case Layout::GENERAL: return VK_IMAGE_LAYOUT_GENERAL;
					case Layout::READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					case Layout::TRANSFER_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					case Layout::TRANSFER_DST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					case Layout::PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
						default: assert(false);
				}
			}(dependency_desc.desiredLayout);
			processResourceAccess(dependency_desc.texDesc, layout, outPass);
		}
		for (const AttachmentDesc& attachment_desc : passDesc.attachmentOperations) {
			const AllocatedTexture& texture = attachment_desc.texDesc.texture.view();
			const VkImageLayout layout = texture.isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			processResourceAccess(attachment_desc.texDesc, layout, outPass);
		}
	}

	void RenderGraph::processAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) {
		if (pass_desc.attachmentOperations.empty()) return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc : pass_desc.attachmentOperations) {
			const TextureDesc& texDesc = attachment_desc.texDesc;
			const AllocatedTexture& allocatedTexture = attachment_desc.texDesc.texture.view();

			const bool isDepthAttachment = vkutil::IsFormatDepth(allocatedTexture.getFormat());
			if (isDepthAttachment) {
				ASSERT_MSG(!hasDepthAttachment,
				           "Pass '{}': Multiple depth attachments not allowed (found '{}' to be a second depth attachment)",
				           outPass.name, allocatedTexture.getDebugName());
				hasDepthAttachment = true;
			}
			const bool needsCustomView = texDesc.baseLayer.has_value() ||
							  texDesc.baseLevel.has_value() ||
							  texDesc.numLayers.has_value() ||
							  texDesc.numLevels.has_value();

			VkImageView imageView;
			if (needsCustomView) {
				const uint32_t baseLayer = texDesc.baseLayer.value_or(0);
				const uint32_t numLayers = texDesc.numLayers.value_or(
					texDesc.baseLayer.has_value() ? 1 : allocatedTexture.getNumLayers()
				);
				const uint32_t baseMip = texDesc.baseLevel.value_or(0);
				const uint32_t numMips = texDesc.numLevels.value_or(
					texDesc.baseLevel.has_value() ? 1 : allocatedTexture.getNumMips()
				);

				auto& mutableTexture = const_cast<Texture&>(texDesc.texture);
				VkImageViewType viewType = allocatedTexture.getViewType();
				if (numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
					viewType = VK_IMAGE_VIEW_TYPE_2D;
				}
				Texture::ViewKey viewKey = mutableTexture.createView({
					.type = viewType,
					.mipLevel = baseMip,
					.numMipLevels = numMips,
					.layer = baseLayer,
					.numLayers = numLayers,
				});

				TextureHandle viewHandle = mutableTexture.handle(viewKey);
				const AllocatedTexture& viewTexture = texDesc.texture._pCtx->view(viewHandle);
				imageView = viewTexture.getImageView();
			} else {
				imageView = allocatedTexture.getImageView();
			}

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
				ASSERT_MSG(resolve_texture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
					"Pass '{}': Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
					pass_desc.name,
					resolve_texture.getDebugName());
				ASSERT_MSG(allocatedTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
					"Pass '{}': Resolve operation on non-multisampled texture '{}'!",
					pass_desc.name, allocatedTexture.getDebugName());
				ASSERT_MSG(resolve_texture.getFormat() == allocatedTexture.getFormat(),
					"Pass '{}': Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
					pass_desc.name,
					allocatedTexture.getDebugName(),
					vkstring::VulkanFormatToString(allocatedTexture.getFormat()),
					resolve_texture.getDebugName(),
					vkstring::VulkanFormatToString(resolve_texture.getFormat())
					);
				if (attachment_desc.storeOp == StoreOp::STORE)
					LOG_SYSTEM_NOSOURCE(LogType::Suggestion, "Pass '{}': Attachment of texture '{}' has StoreOp::STORE and has a resolve attachment, can be replaced with StoreOp::NO_CARE.",
					pass_desc.name,
					allocatedTexture.getDebugName(),
					resolve_texture.getDebugName());

				// set the fields we left empty previously
				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveImageView = resolve_texture.getImageView();
			}
			// detect if attachment is part of swapchain
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
			if (isDepthAttachment) outPass.depthAttachment.emplace(attachment_info);
			else outPass.colorAttachments.emplace_back(attachment_info);

			// calculate the necessary dimensions of the vkCmdBeginRendering call
			// thats all the below code does
			const Dimensions& baseDims = allocatedTexture.getDimensions();
			const uint32_t mipLevel = texDesc.baseLevel.value_or(0);
			const uint32_t width = std::max(1u, baseDims.width >> mipLevel);
			const uint32_t height = std::max(1u, baseDims.height >> mipLevel);

			if ((max_width != baseDims.width || max_height != baseDims.height) && (max_width != 0 || max_height != 0))
				LOG_SYSTEM(LogType::Warning,
			           "Pass '{}': You have attachments of different dimensions, this is allowed but experimental.",
			           pass_desc.name);
			max_width = std::max(max_width, width);
			max_height = std::max(max_height, height);
		}
		outPass.renderArea = {{0, 0}, {max_width, max_height}};
	}

	void RenderGraph::performDryRun(CTX& rCtx) {
		for (const CompiledPass& pass : _compiledPasses) {
			// by default CommandBuffer will have _isDryRun = true
			CommandBuffer dryCmd;
			dryCmd._ctx = &rCtx;
			dryCmd._activePass = pass;
			rCtx._currentCommandBuffer = dryCmd;
#ifdef DEBUG
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, something went horribly wrong!", pass.name);
#endif
			pass.executeCallback(dryCmd);
		}
		rCtx._currentCommandBuffer = {};
	}

	void RenderGraph::compile(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		_compiledPasses.clear();
		_resourceTrackers.clear();
		// compile works in this order
		_compiledPasses.reserve(_passDescriptions.size());
		for (const PassDesc& pass_desc : _passDescriptions) {
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
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
	}

	void RenderGraph::PerformImageBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass) {
		if (compiledPass.imageBarriers.empty()) return;
		ASSERT(cmd._wrapper->_cmdBuf);

		std::vector<VkImageMemoryBarrier2> vkBarriers;
		vkBarriers.reserve(compiledPass.imageBarriers.size());
		// we have to update the image associated if its a swapchain image
		// as it changes every frame
		for (const CompiledImageBarrier& req : compiledPass.imageBarriers) {
			const TextureHandle activeHandle = req.isSwapchain ? cmd._ctx->getCurrentSwapchainTexHandle() : req.handle;
			const AllocatedTexture& activeTexture = cmd._ctx->view(activeHandle);
			if (!_resourceTrackers.contains(activeHandle)) {
				_resourceTrackers.emplace(activeHandle, TextureStateTracker{activeTexture.getNumMips(), activeTexture.getNumLayers()});
			}
			TextureStateTracker& tracker = _resourceTrackers.at(activeHandle);

			if (SubresourceState currentState = tracker.getState(req.range); NeedsBarrier(currentState, req.dstLayout, req.dstMask)) {
				vkBarriers.push_back({
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask = currentState.mask.stage,
					.srcAccessMask = currentState.mask.access,
					.dstStageMask = req.dstMask.stage,
					.dstAccessMask = req.dstMask.access,
					.oldLayout = currentState.layout,
					.newLayout = req.dstLayout,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = activeTexture.getImage(),
					.subresourceRange = MakeVkRange(activeTexture.getFormat(), req.range)
				});
				tracker.setState(req.range, {req.dstLayout, req.dstMask});
				cmd._ctx->access(activeHandle)._vkCurrentImageLayout = req.dstLayout;
			}
		}
		if (!vkBarriers.empty()) {
			const VkDependencyInfo dependencyInfo = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.pNext = nullptr,
				.imageMemoryBarrierCount = static_cast<uint32_t>(vkBarriers.size()),
				.pImageMemoryBarriers = vkBarriers.data()
			};
			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
		}
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");

		for (CompiledPass& pass : _compiledPasses) {
			// perform batched vkCmdPipelineBarrier
			PerformImageBarrierTransitions(cmd, pass);
			// switch color attachment imageViews for Swapchain
		    const uint32_t swapIdx = cmd._ctx->_swapchain->getCurrentImageIndex();
		    for (auto& color : pass.colorAttachments) {
		        if (color.isSwapchainImage)
		            color.imageView = color.swapchainImageViews[swapIdx];
		    }
		    if (pass.depthAttachment && pass.depthAttachment->isSwapchainImage)
				pass.depthAttachment->imageView = pass.depthAttachment->swapchainImageViews[swapIdx];
			// reset current states
			cmd._currentPipelineInfo = nullptr;
			cmd._currentPipelineHandle = {};
			cmd._activePass = pass;
			pass.executeCallback(cmd);
		}
		// fixme: make this cleaner im so lazy right now
		cmd.cmdTransitionLayout(cmd._ctx->getCurrentSwapchainTexHandle(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		_resourceTrackers.at(cmd._ctx->getCurrentSwapchainTexHandle()).setState({}, SubresourceState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, dstMask});
		cmd._ctx->access(cmd._ctx->getCurrentSwapchainTexHandle())._vkCurrentImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}
}