//
// Created by Hayden Rivas on 10/11/25.
//

#include "CommandBuffer.h"
#include "vkinfo.h"
#include "vkenums.h"
#include "CTX.h"

#include "mythril/RenderGraphBuilder.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {

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
		if (currentStateWrites || IsAttachmentImageLayout(desiredLayout))
			return true;

		if (currentState.mask.stage != desiredStageAccess.stage) {
			return true;
		}
		return false;
	}

	void RenderGraph::processResourceAccess(const CTX& rCtx, const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass) {
		const TextureHandle handle = texDesc.handle;
		const AllocatedTexture& texture = rCtx.view(handle);

		// make sure a tracker for given handle exists
		if (!resourceTrackers.contains(texDesc.handle)) {
			resourceTrackers.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(texDesc.handle),
				std::forward_as_tuple(texture.getNumMips(), texture.getNumLayers())
			);
		}
		TextureStateTracker& tracker = resourceTrackers.at(texDesc.handle);
		const SubresourceRange range = {
			.baseMip = texDesc.baseLevel.value_or(0),
			.numMips = texDesc.numLevels.value_or(texture.getNumMips()),
			.baseLayer = texDesc.baseLayer.value_or(0),
			.numLayers = texDesc.numLayers.value_or(texture.getNumLayers())
		};

		// SubresourceState includes StageAccess
		const SubresourceState currentState = tracker.getState(range);
		const vkutil::StageAccess desiredMask = vkutil::GetPipelineStageAccess(desiredLayout);

		if (NeedsBarrier(currentState, desiredLayout, desiredMask)) {
			const VkImageMemoryBarrier2 barrier2 = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.pNext = nullptr,
				.srcStageMask = currentState.mask.stage,
				.srcAccessMask = currentState.mask.access,
				.dstStageMask = desiredMask.stage,
				.dstAccessMask = desiredMask.access,
				.oldLayout = currentState.layout,
				.newLayout = desiredLayout,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = texture.getImage(),
				.subresourceRange = MakeVkRange(texture.getFormat(), range)
			};
			outPass.imageBarriers.emplace_back(CompiledImageBarrier{handle, barrier2});
		}
		tracker.setState(range, SubresourceState{desiredLayout, desiredMask});
	}

	void RenderGraph::processPassResources(CTX& rCtx, const PassDesc& passDesc, CompiledPass& outPass) {
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
			processResourceAccess(rCtx, dependency_desc.texDesc, layout, outPass);
		}
		for (const AttachmentDesc& attachment_desc : passDesc.attachmentOperations) {
			const AllocatedTexture& texture = rCtx.view(attachment_desc.texDesc.handle);
			const VkImageLayout layout = texture.isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			processResourceAccess(rCtx, attachment_desc.texDesc, layout, outPass);
		}
	}

	void RenderGraph::processAttachments(CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) {
		if (pass_desc.attachmentOperations.empty()) return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc : pass_desc.attachmentOperations) {
			const AllocatedTexture& texture = rCtx.view(attachment_desc.texDesc.handle);

			const bool isDepthAttachment = vkutil::IsFormatDepth(texture.getFormat());
			if (isDepthAttachment) {
				ASSERT_MSG(!hasDepthAttachment,
				           "Pass '{}': Multiple depth attachments not allowed (found '{}' to be a second depth attachment)",
				           outPass.name, texture.getDebugName());
				hasDepthAttachment = true;
			}

			const ClearValue& clear_value = attachment_desc.clearValue;
			AttachmentInfo attachment_info = {
				.imageFormat = texture.getFormat(),
				.imageView = texture.getImageView(),
				.imageLayout = isDepthAttachment ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				// these fields are "resolved" in the next steps, haha
				.resolveImageView = VK_NULL_HANDLE,
				.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,

				.loadOp = toVulkan(attachment_desc.loadOp),
				.storeOp = toVulkan(attachment_desc.storeOp),
				.clearValue = isDepthAttachment ? clear_value.getDepthStencilValue() : clear_value.getColorValue()
			};
			if (attachment_desc.resolveTexDesc.has_value()) {
				const TextureDesc& resolve_desc = attachment_desc.resolveTexDesc.value();
				const AllocatedTexture& resolve_texture = rCtx.view(resolve_desc.handle);
				ASSERT_MSG(resolve_texture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
					"Pass '{}': Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
					pass_desc.name,
					resolve_texture.getDebugName());
				ASSERT_MSG(texture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
					"Pass '{}': Resolve operation on non-multisampled texture '{}'!",
					pass_desc.name, texture.getDebugName());
				ASSERT_MSG(resolve_texture.getFormat() == texture.getFormat(),
					"Pass '{}': Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
					pass_desc.name,
					texture.getDebugName(),
					vkstring::VulkanFormatToString(texture.getFormat()),
					resolve_texture.getDebugName(),
					vkstring::VulkanFormatToString(resolve_texture.getFormat())
					);

				// set the fields we left empty previously
				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveImageView = resolve_texture.getImageView();
			}

			if (isDepthAttachment) outPass.depthAttachment.emplace(attachment_info);
			else outPass.colorAttachments.emplace_back(attachment_info);

			const Dimensions& dims = texture.getDimensions();
			// second logical statement protects againt the first iteration
			if ((max_width != dims.width || max_height != dims.height) && (max_width != 0 || max_height != 0))
				LOG_SYSTEM(LogType::Warning,
			           "Pass '{}': You have attachments of different dimensions, this is allowed but experimental.",
			           pass_desc.name);
			max_width = std::max(max_width, dims.width);
			max_height = std::max(max_height, dims.height);
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
		// compile works in this order
		_compiledPasses.reserve(_passDescriptions.size());
		for (const PassDesc& pass_desc : _passDescriptions) {
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.type = pass_desc.type;
			compiled_pass.executeCallback = pass_desc.executeCallback;

			// do not worry about the return value,
			// every type of process should run for every pass
			processPassResources(rCtx, pass_desc, compiled_pass);
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
		for (const CompiledImageBarrier& image_barrier : compiledPass.imageBarriers) {
			VkImageMemoryBarrier2 vk_image_memory_barrier2 = image_barrier.barrier;
			if (cmd._ctx->view(image_barrier.textureHandle).isSwapchainImage())
				vk_image_memory_barrier2.image = cmd._ctx->view(cmd._ctx->getCurrentSwapchainTex()).getImage();
			vkBarriers.push_back(vk_image_memory_barrier2);
		}

		const VkDependencyInfo dependencyInfo = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.pNext = nullptr,
			.imageMemoryBarrierCount = static_cast<uint32_t>(compiledPass.imageBarriers.size()),
			.pImageMemoryBarriers = vkBarriers.data()
		};
		vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		for (const CompiledPass& pass : _compiledPasses) {
			// reset current states
			PerformImageBarrierTransitions(cmd, pass);
			cmd._currentPipelineInfo = nullptr;
			cmd._currentPipelineHandle = {};
			cmd._activePass = pass;
			pass.executeCallback(cmd);
		}
	}

// 	void RenderGraph::compile(CTX& rCtx) {
// 		MYTH_PROFILER_ZONE("RenderGraph::Compile", MYTH_PROFILER_COLOR_RENDERGRAPH);
// 		this->_compiledPasses.clear();
//
// 		// BUILD PASS DESCRIPTIONS -> PIPELINE BARRIERS //
// 		for (const PassDesc& source: this->_passDescriptions) {
// 			CompiledPass compiled;
// 			compiled.name = source.name;
// 			compiled.type = source.type;
// 			compiled.executeCallback = source.executeCallback;
//
// 			// STEP 1: PROCESS READ OPERATIONS
// 			for (const ReadSpec& readOperation : source.readOperations) {
// 				compiled.preBarriers.emplace_back(CompileReadOperation(rCtx, readOperation));
// 			}
//
// 			// STEP 2: PROCESS WRITE OPERATIONS
// 			if (source.type == PassDesc::Type::Graphics) {
// 				ASSERT_MSG(!source.writeOperations.empty(),
// 						   "Graphics Pass '{}' has no write operations, which is not allowed.",
// 						   source.name);
//
// 				VkExtent2D referenceExtent2D = rCtx.view(source.writeOperations.front().texture).getExtentAs2D();
// 				compiled.extent2D = referenceExtent2D;
//
// #ifdef DEBUG
// 				ValidateWriteOperationResolutions(rCtx, source, referenceExtent2D);
// #endif
//
// 				bool hasDepthAttachment = false;
// 				for (const WriteSpec& writeOperation: source.writeOperations) {
// 					// get .texture associated with write()
// 					const AllocatedTexture& currentTexture = rCtx.view(writeOperation.texture);
// 					ASSERT_MSG(currentTexture._vkImage != VK_NULL_HANDLE,
// 							   "Pass '{}': Texture write operation references invalid vkImage for '{}'",
// 							   source.name, currentTexture.getDebugName());
//
// 					// first check of writes is if its depth
// 					if (currentTexture.isDepthAttachment()) {
// 						ASSERT_MSG(!hasDepthAttachment,
// 								   "Pass '{}': Multiple depth attachments not allowed (found '{}')",
// 								   source.name, currentTexture.getDebugName());
// 						hasDepthAttachment = true;
//
// 						// Depth attachment info
// 						DepthAttachmentInfo depthInfo = {
// 							.imageView = currentTexture._vkImageView,
// 							.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
// 							.imageFormat = currentTexture._vkFormat,
// 							// if resolve target is included, fill in the following steps
// 							.resolveImageView = VK_NULL_HANDLE,
// 							.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
//
// 							.loadOp = toVulkan(writeOperation.loadOp),
// 							.storeOp = toVulkan(writeOperation.storeOp),
// 							.clearDepthStencil = writeOperation.clearValue.clearDepthStencil.getAsVkClearDepthStencilValue()
// 						};
//
// 						// create barrier for depth attachment
// 						constexpr VkImageLayout newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
// 						compiled.preBarriers.push_back(
// 							CompileWriteOperation(rCtx, writeOperation, newLayout)
// 						);
// 						// Handle MSAA Depth resolve
// 						if (writeOperation.resolveTexture.has_value()) {
// 							const AllocatedTexture& resolveTexture = rCtx.view(writeOperation.resolveTexture.value());
// 							ASSERT_MSG(resolveTexture._vkImage != VK_NULL_HANDLE,
// 									   "Pass '{}': Resolve target '{}' for '{}' has invalid vkImage!",
// 									   source.name, resolveTexture.getDebugName(), currentTexture.getDebugName());
// 							ASSERT_MSG(currentTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
// 									   "Pass '{}': Resolve operation on non-multisampled texture '{}'!",
// 									   source.name, currentTexture.getDebugName());
// 							ASSERT_MSG(resolveTexture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
// 									   "Pass '{}': Resolve target '{}' is multisampled!",
// 									   source.name, resolveTexture.getDebugName());
//
// 							depthInfo.resolveImageView = resolveTexture._vkImageView;
// 							depthInfo.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
//
// 							// create barrier for resolve target
// 							VkImageMemoryBarrier2 resolveBarrier = vkinfo::CreateImageMemoryBarrier2(
// 									resolveTexture._vkImage,
// 									resolveTexture._vkFormat,
// 									VK_IMAGE_LAYOUT_UNDEFINED,  // PLACEHOLDER
// 									VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
// 									true);
// 							compiled.preBarriers.push_back({resolveBarrier, writeOperation.resolveTexture.value(), false, true});
// 						}
// 						// set it
// 						compiled.depthAttachment = depthInfo;
// 					} else {
// 						// Color attachment info
// 						ColorAttachmentInfo colorInfo = {
// 							.imageView = currentTexture._vkImageView,
// 							.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
// 							.imageFormat = currentTexture._vkFormat,
// 							// if resolve target is included, fill in the following steps
// 							.resolveImageView = VK_NULL_HANDLE,
// 							.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
// 							.loadOp = toVulkan(writeOperation.loadOp),
// 							.storeOp = toVulkan(writeOperation.storeOp),
// 							.clearColor = writeOperation.clearValue.clearColor.getAsVkClearColorValue(),
// 						};
// 						// Create barrier for color attachment
// 						constexpr VkImageLayout newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
// 						compiled.preBarriers.push_back(
// 							CompileWriteOperation(rCtx, writeOperation, newLayout)
// 						);
//
// 						// Handle MSAA Color resolve
// 						if (writeOperation.resolveTexture.has_value()) {
// 							const AllocatedTexture& resolveTexture = rCtx.view(writeOperation.resolveTexture.value());
//
// 							ASSERT_MSG(resolveTexture._vkImage != VK_NULL_HANDLE,
// 									   "Pass '{}': Resolve target '{}' for '{}' has invalid vkImage!",
// 									   source.name, resolveTexture.getDebugName(), currentTexture.getDebugName());
// 							ASSERT_MSG(currentTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
// 									   "Pass '{}': Resolve operation on non-multisampled texture '{}'!",
// 									   source.name, currentTexture.getDebugName());
// 							ASSERT_MSG(resolveTexture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
// 									   "Pass '{}': Resolve target '{}' is multisampled!",
// 									   source.name, resolveTexture.getDebugName());
//
// 							colorInfo.resolveImageView = resolveTexture._vkImageView;
// 							colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
//
// 							// create barrier for resolve target
// 							VkImageMemoryBarrier2 resolveBarrier = vkinfo::CreateImageMemoryBarrier2(
// 									resolveTexture._vkImage,
// 									resolveTexture._vkFormat,
// 									VK_IMAGE_LAYOUT_UNDEFINED,  // PLACEHOLDER
// 									VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
// 									true);
// 							compiled.preBarriers.push_back({resolveBarrier, writeOperation.resolveTexture.value(), false, true});
// 						}
//
// 						compiled.colorAttachments.push_back(colorInfo);
// 					}
// 				}
//
// 				ASSERT_MSG(!compiled.colorAttachments.empty() || hasDepthAttachment,
// 						   "Pass '{}' was given no color or depth attachments!", source.name);
// 			}
//
// 			this->_compiledPasses.push_back(std::move(compiled));
// 		}
//
// 		// BUILD RENDER PIPELINES & other commands //
// 		// This runs the callbacks in a "dry run" mode to let them create pipelines
// 		for (const CompiledPass& pass : this->_compiledPasses) {
// 			CommandBuffer dryCmd;
// 			dryCmd._ctx = &rCtx;
// 			dryCmd._activePass = pass;
// 			rCtx._currentCommandBuffer = dryCmd;
// 			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, something went horribly wrong!", pass.name);
// 			pass.executeCallback(dryCmd);
// 		}
// 		rCtx._currentCommandBuffer = {};
//
// 		_hasCompiled = true;
// 		MYTH_PROFILER_ZONE_END();
// 	}
//
// 	void RenderGraph::PerformTransitions(CommandBuffer& cmd, const CompiledPass& currentPass) {
// 		std::vector<VkImageMemoryBarrier2> barriers;
// 		barriers.reserve(currentPass.preBarriers.size());
// 		CTX& ctx = *cmd._ctx;
//
// 		for (const CompiledBarrier& compiled_bi : currentPass.preBarriers) {
// 			VkImageMemoryBarrier2 barrier = compiled_bi.barrier;
// 			const AllocatedTexture& tex = ctx.view(compiled_bi.textureHandle);
// 			barrier.oldLayout = tex.getImageLayout();
//
// 			if (barrier.oldLayout == barrier.newLayout) {
// 				continue;
// 			}
// 			// LOG_SYSTEM(LogType::Info, "For Pass '{}': Barrier for '{}': {} -> {}",
// 			// 	currentPass.name,
// 			//   tex.getDebugName(),
// 			//   vkstring::VulkanImageLayoutToString(barrier.oldLayout),
// 			//   vkstring::VulkanImageLayoutToString(barrier.newLayout));
//
// 			barriers.push_back(barrier);
// 		}
//
// 		if (!barriers.empty()) {
// 			ASSERT(cmd._wrapper->_cmdBuf);
// 			VkDependencyInfo dependencyInfo = {
// 				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
// 				.pNext = nullptr,
// 				.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
// 				.pImageMemoryBarriers = barriers.data()
// 			};
// 			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
//
// 			// Update tracked layouts - use the barriers we actually submitted!
// 			for (const VkImageMemoryBarrier2& barrier : barriers) {
// 				// Find the texture handle that corresponds to this barrier's image
// 				for (const CompiledBarrier& cb : currentPass.preBarriers) {
// 					const AllocatedTexture& tex = ctx.view(cb.textureHandle);
// 					if (tex.getImage() == barrier.image) {
// 						ctx._texturePool.get(cb.textureHandle)->_vkCurrentImageLayout = barrier.newLayout;
// 						break;
// 					}
// 				}
// 			}
// 		}
// 	}
//
// 	void RenderGraph::execute(CommandBuffer& cmd) {
// 		MYTH_PROFILER_ZONE("RenderGraph::execute", MYTH_PROFILER_COLOR_RENDERGRAPH);
// 		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
// 		ASSERT_MSG(this->_hasCompiled, "RenderGraph must be compiled before it can be executed!");
// 		if (this->_compiledPasses.empty()) {
// 			LOG_SYSTEM(LogType::Warning, "RenderGraph has no passes, execute will do nothing!");
// 			return;
// 		}
// 		// execute each pass in order of added
// 		for (const CompiledPass& pass : _compiledPasses) {
// 			// reset current states
// 			cmd._currentPipelineInfo = nullptr;
// 			cmd._currentPipelineHandle = {};
// 			cmd._activePass = pass;
// 			// if barriers are required
// 			MYTH_PROFILER_ZONE("Transitions", MYTH_PROFILER_COLOR_RENDERGRAPH);
// 			PerformTransitions(cmd, pass);
// 			MYTH_PROFILER_ZONE_END();
// // #ifdef DEBUG
// // 			ValidateReadImageLayouts(*cmd._ctx, this->_sourcePasses);
// // #endif
// 			// execute the pass callback
// 			MYTH_PROFILER_ZONE(pass.name.c_str(), MYTH_PROFILER_COLOR_RENDERPASS);
// 			pass.executeCallback(cmd);
// 			MYTH_PROFILER_ZONE_END();
// 		}
// 		MYTH_PROFILER_ZONE_END();
// 	}
}