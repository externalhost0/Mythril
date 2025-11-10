//
// Created by Hayden Rivas on 10/11/25.
//


#include "CommandBuffer.h"
#include "vkinfo.h"
#include "vkenums.h"
#include "CTX.h"

#include "mythril/RenderGraphBuilder.h"
#include "vkutil.h"
#include "Logger.h"

namespace mythril {

	RenderPassBuilder& RenderPassBuilder::write(WriteSpec spec) {
		// we will save the data and cannot actually use it until we do some processing for more information in compile()
		passSource.writeOperations.push_back(spec);
		return *this;
	}
	RenderPassBuilder& RenderPassBuilder::read(ReadSpec spec) {
		passSource.readOperations.push_back(spec);
		return *this;
	}

	void RenderPassBuilder::setExecuteCallback(const std::function<void(CommandBuffer&)>& callback) {
		this->passSource.executeCallback = callback;
		this->_graphRef._sourcePasses.push_back(std::move(passSource));
		// reset status incase user compiled and than adds another pass
		this->_graphRef._hasCompiled = false;
	}

	void RenderGraph::compile(CTX& ctx) {
		// remove past compilations
		this->_compiledPasses.clear();

		// BUILD PASS DESCRIPTIONS -> PIPELINE BARRIERS //
		for (const PassSource& source: this->_sourcePasses) {
			ASSERT_MSG(!source.writeOperations.empty(), "Pass '{}' has no write operations, what does this pass even do than?", source.name);

			PassCompiled compiled;
			// first set some basic info
			compiled.name = source.name;
			compiled.type = source.type;
			compiled.executeCallback = source.executeCallback;
			// TODO: scuffed way to find extent for renderpass
			VkExtent2D refrenceExtent2D = ctx.viewTexture(source.writeOperations.front().texture).getExtentAs2D();
			compiled.extent2D = refrenceExtent2D;

			// Validate all attached textures
#if defined(DEBUG)
			for (size_t i = 0; i < source.writeOperations.size(); ++i) {
				const WriteSpec& writeOp = source.writeOperations[i];
				const AllocatedTexture& texture = ctx.viewTexture(writeOp.texture);
				VkExtent2D extent = texture.getExtentAs2D();

				// check all writeOp.textures
				ASSERT_MSG(extent.width == refrenceExtent2D.width && extent.height == refrenceExtent2D.height,
					   "Pass '{}': Write attachment {} ('{}') has mismatched dimensions {}x{}, expected {}x{} (reference: '{}')",
					   source.name, i, texture._debugName,
					   extent.width, extent.height, refrenceExtent2D.width, refrenceExtent2D.height,
					   ctx.viewTexture(source.writeOperations.front().texture)._debugName);

				// check all writeOp.resolveTextures
				if (writeOp.resolveTexture.has_value()) {
					const AllocatedTexture& resolveTexture = ctx.viewTexture(writeOp.resolveTexture.value());
					VkExtent2D resolveExtent = resolveTexture.getExtentAs2D();

					ASSERT_MSG(resolveExtent.width == refrenceExtent2D.width && resolveExtent.height == refrenceExtent2D.height,
						   "Pass '{}': Resolve attachment for '{}' has mismatched dimensions {}x{}, expected {}x{}",
						   source.name, texture._debugName,
						   resolveExtent.width, resolveExtent.height,
						   refrenceExtent2D.width, refrenceExtent2D.height);
				}
			}
#endif

			// STEP 1: PROCESS READ OPERATIONS
			for (const ReadSpec& readOperation: source.readOperations) {
				const AllocatedTexture& currentTexture = ctx.viewTexture(readOperation.texture);

				ASSERT_MSG(currentTexture._vkImage != VK_NULL_HANDLE, "Pass '{}': Texture read operation references invalid vkImage for '{}'", source.name, currentTexture._debugName);

				VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
						currentTexture._vkImage,
						currentTexture._vkFormat,
						currentTexture._vkCurrentImageLayout,
						readOperation.expectedLayout,
						false);
				compiled.preBarriers.push_back({barrier, readOperation.texture});
			}

			// tracking depth attachment status
			bool hasDepthAttachment = false;
			// STEP 2: PROCESS WRITE OPERATIONS
			for (const WriteSpec& writeOperation: source.writeOperations) {
				const AllocatedTexture& currentTexture = ctx.viewTexture(writeOperation.texture);
				ASSERT_MSG(currentTexture._vkImage != VK_NULL_HANDLE, "Pass '{}': Texture write operation references invalid vkImage for '{}'", source.name, currentTexture._debugName);

				if (currentTexture.isDepthAttachment()) {
					ASSERT_MSG(!hasDepthAttachment, "Pass '{}': Multiple depth attachments not allowed (found '{}')", source.name, currentTexture._debugName);
					// FOR DEPTH ATTACHMENTS //
					// we dont have to do much processing for DepthAttachments so just assign directly
					compiled.depthAttachment = DepthAttachmentInfo{
							.imageView = currentTexture._vkImageView,
							.imageLayout = currentTexture._vkCurrentImageLayout,
							.imageFormat = currentTexture._vkFormat,
							.loadOp = toVulkan(writeOperation.loadOp),
							.storeOp = toVulkan(writeOperation.storeOp),
							.clearDepthStencil = writeOperation.clearValue.clearDepthStencil.getAsVkClearDepthStencilValue()
					};

					VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
							currentTexture._vkImage,
							currentTexture._vkFormat,
							currentTexture._vkCurrentImageLayout,
							VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							false);
					compiled.preBarriers.push_back({barrier, writeOperation.texture});
				} else {
					// FOR COLOR ACTTACHMENTS //
					ColorAttachmentInfo colorInfo = {
							.imageView = currentTexture._vkImageView,
							.imageLayout = currentTexture._vkCurrentImageLayout,
							.imageFormat = currentTexture._vkFormat,

							// this is filled in the following steps if needed
							.resolveImageView = VK_NULL_HANDLE,
							.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,

							.loadOp = toVulkan(writeOperation.loadOp),
							.storeOp = toVulkan(writeOperation.storeOp),
							.clearColor = writeOperation.clearValue.clearColor.getAsVkClearColorValue(),
					};
					VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
							currentTexture._vkImage,
							currentTexture._vkFormat,
							currentTexture._vkCurrentImageLayout,
							VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							false);
					compiled.preBarriers.push_back({barrier, writeOperation.texture});

					// keep setting last texture, last set is the last duh
					_lastColorTexture = writeOperation.texture;

					// if color attachment is to resolve onto another
					if (writeOperation.resolveTexture.has_value()) {
						const AllocatedTexture& resolveTexture = ctx.viewTexture(writeOperation.resolveTexture.value());
						// some checks, common mistakes
						ASSERT_MSG(resolveTexture._vkImage != VK_NULL_HANDLE, "Pass '{}': Resolve target '{}' for '{}' has invalid vkImage!", source.name, resolveTexture._debugName, currentTexture._debugName);
						ASSERT_MSG(currentTexture._vkSampleCountFlagBits > VK_SAMPLE_COUNT_1_BIT, "Pass '{}': Resolve operation on non-multisampled texture '{}'!", source.name, currentTexture._debugName);
						ASSERT_MSG(resolveTexture._vkSampleCountFlagBits == VK_SAMPLE_COUNT_1_BIT, "Pass '{}': Resolve target '{}' is multisampled!", source.name, resolveTexture._debugName);

						colorInfo.resolveImageView = resolveTexture._vkImageView;
						// resolve targets should always be in this layout before rendering, as far as i know
						colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

						// and if its a resolve set it after the other set
						_lastColorTexture = writeOperation.resolveTexture.value();

						// make the resolve target also in the optimal layout
						VkImageMemoryBarrier2 resolveBarrier = vkinfo::CreateImageMemoryBarrier2(
								resolveTexture._vkImage,
								resolveTexture._vkFormat,
								resolveTexture._vkCurrentImageLayout,
								VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								true);
						compiled.preBarriers.push_back({resolveBarrier, writeOperation.resolveTexture.value()});
					}
					compiled.colorAttachments.push_back(colorInfo);
				}
			}
			ASSERT_MSG(!compiled.colorAttachments.empty() || hasDepthAttachment, "Pass '{}' has no color or depth attachments!", source.name);
			this->_compiledPasses.push_back(std::move(compiled));
		}
		// once done transforming PassSource -> PassCompiled we still need sourcePasses incase we recompile

		// BUILD RENDER PIPELINES //
		// TODO: this is horrible, dont have much else to say
		for (const PassCompiled& pass : _compiledPasses) {
			// this is a dummy command that will not issue any vulkan related commands itself
			CommandBuffer dryCmd;
			dryCmd._ctx = &ctx;
			dryCmd._activePass = pass;
			ctx._currentCommandBuffer = dryCmd;
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' has null execute callback, how is that possible??", pass.name);
			pass.executeCallback(dryCmd);
		}
		ctx._currentCommandBuffer = {};

		_hasCompiled = true;
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");

		for (const PassCompiled& pass : _compiledPasses) {
			cmd._activePass = pass;

			if (!pass.preBarriers.empty()) {
				std::vector<VkImageMemoryBarrier2> barriers;
				barriers.reserve(pass.preBarriers.size());
				for (const CompiledBarrier& cb : pass.preBarriers) {
					barriers.push_back(cb.barrier);
				}

				VkDependencyInfo dependencyInfo = {
						.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
						.pNext = nullptr,
						.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
						.pImageMemoryBarriers = barriers.data()
				};
				vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
				for (const CompiledBarrier& cb : pass.preBarriers) {
					cmd._ctx->_texturePool.get(cb.textureHandle)->_vkCurrentImageLayout = cb.barrier.newLayout;
				}
			}

			cmd.cmdBeginRenderingImpl();
			pass.executeCallback(cmd);
			cmd.cmdEndRenderingImpl();
		}
		cmd.cmdPrepareToSwapchainImpl(_lastColorTexture);
	}
}