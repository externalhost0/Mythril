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
#include "vkstring.h"

namespace mythril {
	// *PassBuilders and their respective operations + the always necessary setExecuteCallback
	GraphicsPassBuilder& GraphicsPassBuilder::write(const WriteSpec &spec) {
		// we will save the data and cannot actually use it until we do some processing for more information in compile()
		this->_passSource.writeOperations.push_back(spec);
		return *this;
	}
	GraphicsPassBuilder& GraphicsPassBuilder::read(const ReadSpec& spec) {
		this->_passSource.readOperations.push_back(spec);
		return *this;
	}
	void GraphicsPassBuilder::setExecuteCallback(const std::function<void(CommandBuffer &)> &callback) {
		this->_passSource.executeCallback = callback;
		this->_graphRef._sourcePasses.push_back(_passSource);
		// reset status incase user compiled and than adds another pass
		this->_graphRef._hasCompiled = false;
	}

	ComputePassBuilder& ComputePassBuilder::read(const ReadSpec& spec) {
		this->_passSource.readOperations.push_back(spec);
		return *this;
	}
	void ComputePassBuilder::setExecuteCallback(const std::function<void(CommandBuffer &)> &callback) {
		this->_passSource.executeCallback = callback;
		this->_graphRef._sourcePasses.push_back(_passSource);
		this->_graphRef._hasCompiled = false;

	}

	// checks if all textures in writeSpec are the same resolution
	static void ValidateWriteOperationResolutions(const CTX& ctx, const PassSource& source, VkExtent2D refrenceExtent2D) {
		// alias
		const std::vector<WriteSpec> writeOperations = source.writeOperations;
		for (size_t i = 0; i < writeOperations.size(); ++i) {
			const WriteSpec &writeOp = writeOperations[i];
			const AllocatedTexture &texture = ctx.viewTexture(writeOp.texture);
			const VkExtent2D extent = texture.getExtentAs2D();

			// check all writeOp.textures
			ASSERT_MSG(extent.width == refrenceExtent2D.width && extent.height == refrenceExtent2D.height,
					   "Pass '{}': Write attachment {} ('{}') has mismatched dimensions {}x{}, expected {}x{} (reference: '{}')",
					   source.name, i, texture.getDebugName(),
					   extent.width, extent.height, refrenceExtent2D.width, refrenceExtent2D.height,
					   ctx.viewTexture(source.writeOperations.front().texture).getDebugName());

			// check all writeOp.resolveTextures
			if (writeOp.resolveTexture.has_value()) {
				const AllocatedTexture &resolveTexture = ctx.viewTexture(writeOp.resolveTexture.value());
				const VkExtent2D resolveExtent = resolveTexture.getExtentAs2D();

				ASSERT_MSG(resolveExtent.width == refrenceExtent2D.width &&
						   resolveExtent.height == refrenceExtent2D.height,
						   "Pass '{}': Resolve attachment for '{}' has mismatched dimensions {}x{}, expected {}x{}",
						   source.name, texture.getDebugName(),
						   resolveExtent.width, resolveExtent.height,
						   refrenceExtent2D.width, refrenceExtent2D.height);
			}
		}
	}

	CompiledBarrier CompileReadOperation(CTX& ctx, const ReadSpec& readSpec) {
		const AllocatedTexture& currentTexture = ctx.viewTexture(readSpec.texture);
		ASSERT_MSG(currentTexture.getImage() != VK_NULL_HANDLE, "Texture read operation could not be compiled for '{}'!", currentTexture.getDebugName());

		const VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
				currentTexture.getImage(),
				currentTexture.getFormat(),
				currentTexture.getImageLayout(),
				readSpec.expectedLayout,
				false);
		return {barrier, readSpec.texture};
	}
	CompiledBarrier CompileWriteOperation(CTX& ctx, const WriteSpec& writeSpec, VkImageLayout vkNewImageLayout) {
		const AllocatedTexture& currentTexture = ctx.viewTexture(writeSpec.texture);
		ASSERT_MSG(currentTexture.getImage() != VK_NULL_HANDLE, "Texture read operation could not be compiled for '{}'!", currentTexture.getDebugName());

		VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
				currentTexture.getImage(),
				currentTexture.getFormat(),
				currentTexture.getImageLayout(),
				vkNewImageLayout,
				false);

		return {barrier, writeSpec.texture};
	}

	// todo: please compact and split this giant function up, especiall that if statement for PassSource::Type
	void RenderGraph::compile(CTX& ctx) {
		// remove past compilations
		this->_compiledPasses.clear();

		// BUILD PASS DESCRIPTIONS -> PIPELINE BARRIERS //
		for (const PassSource& source: this->_sourcePasses) {
			PassCompiled compiled;
			// first set some basic info
			compiled.name = source.name;
			compiled.type = source.type;
			compiled.executeCallback = source.executeCallback;

			// STEP 1: PROCESS READ OPERATIONS
			for (const ReadSpec& readOperation: source.readOperations) {
				compiled.preBarriers.emplace_back(CompileReadOperation(ctx, readOperation));
				// update layout during compile, as later passes may read an images layout
				ctx._texturePool.get(readOperation.texture)->_vkCurrentImageLayout = readOperation.expectedLayout;
			}


			// STEP 2: PROCESS WRITE OPERATIONS
			if (source.type == PassSource::Type::Graphics) {
				ASSERT_MSG(!source.writeOperations.empty(),
						   "Graphics Pass '{}' has no write operations, which is not allowed.",
						   source.name);

				// TODO: scuffed way to find extent for renderpass
				VkExtent2D refrenceExtent2D = ctx.viewTexture(source.writeOperations.front().texture).getExtentAs2D();
				compiled.extent2D = refrenceExtent2D;

#ifdef DEBUG
				// Validate all attached textures resolution
				ValidateWriteOperationResolutions(ctx, source, refrenceExtent2D);
#endif
				// tracking depth attachment status
				bool hasDepthAttachment = false;
				for (const WriteSpec& writeOperation: source.writeOperations) {
					const AllocatedTexture &currentTexture = ctx.viewTexture(writeOperation.texture);
					ASSERT_MSG(currentTexture._vkImage != VK_NULL_HANDLE,
							   "Pass '{}': Texture write operation references invalid vkImage for '{}'",
							   source.name,
							   currentTexture.getDebugName());

					if (currentTexture.isDepthAttachment()) {
						ASSERT_MSG(!hasDepthAttachment,
								   "Pass '{}': Multiple depth attachments not allowed (found '{}')", source.name,
								   currentTexture.getDebugName());
						hasDepthAttachment = true;
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

						VkImageLayout vk_new_image_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
						VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
								currentTexture._vkImage,
								currentTexture._vkFormat,
								currentTexture._vkCurrentImageLayout,
								vk_new_image_layout,
								false);
						compiled.preBarriers.push_back({barrier, writeOperation.texture});
						ctx._texturePool.get(writeOperation.texture)->_vkCurrentImageLayout = vk_new_image_layout;
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
						VkImageLayout vk_new_image_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
						VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
								currentTexture._vkImage,
								currentTexture._vkFormat,
								currentTexture._vkCurrentImageLayout,
								vk_new_image_layout,
								false);
						compiled.preBarriers.push_back({barrier, writeOperation.texture});
						ctx._texturePool.get(writeOperation.texture)->_vkCurrentImageLayout = vk_new_image_layout;

						// keep setting last texture, last set is the last duh
						this->_lastColorTexture = writeOperation.texture;

						// if color attachment is to resolve onto another
						if (writeOperation.resolveTexture.has_value()) {
							const AllocatedTexture &resolveTexture = ctx.viewTexture(
									writeOperation.resolveTexture.value());
							// some checks, common mistakes
							ASSERT_MSG(resolveTexture._vkImage != VK_NULL_HANDLE,
									   "Pass '{}': Resolve target '{}' for '{}' has invalid vkImage!", source.name,
									   resolveTexture.getDebugName(), currentTexture.getDebugName());
							ASSERT_MSG(currentTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
									   "Pass '{}': Resolve operation on non-multisampled texture '{}'!",
									   source.name,
									   currentTexture.getDebugName());
							ASSERT_MSG(resolveTexture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
									   "Pass '{}': Resolve target '{}' is multisampled!", source.name,
									   resolveTexture.getDebugName());

							colorInfo.resolveImageView = resolveTexture._vkImageView;
							// resolve targets should always be in this layout before rendering, as far as i know
							colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

							// and if its a resolve set it after the other set
							this->_lastColorTexture = writeOperation.resolveTexture.value();

							// make the resolve target also in the optimal layout
							VkImageMemoryBarrier2 resolveBarrier = vkinfo::CreateImageMemoryBarrier2(
									resolveTexture._vkImage,
									resolveTexture._vkFormat,
									resolveTexture._vkCurrentImageLayout,
									vk_new_image_layout,
									true);
							compiled.preBarriers.push_back({resolveBarrier, writeOperation.resolveTexture.value()});
							ctx._texturePool.get(
									writeOperation.resolveTexture.value())->_vkCurrentImageLayout = vk_new_image_layout;
						}
						compiled.colorAttachments.push_back(colorInfo);
					}
				}
				ASSERT_MSG(!compiled.colorAttachments.empty() || hasDepthAttachment, "Pass '{}' was given no color or depth attachments!", source.name);
			}

			// after pass is done being transformed, push it back
			this->_compiledPasses.push_back(std::move(compiled));
		}
		// once done transforming PassSource -> PassCompiled we still need to store sourcePasses incase we recompile

		// BUILD RENDER PIPELINES //
		// TODO: this is horrible, dont have much else to say
		for (const PassCompiled& pass : this->_compiledPasses) {
			// this is a dummy command that will not issue any vulkan related commands itself
			CommandBuffer dry_cmd;
			dry_cmd._ctx = &ctx;
			dry_cmd._activePass = pass;
			ctx._currentCommandBuffer = dry_cmd;
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, how is that possible??", pass.name);
			pass.executeCallback(dry_cmd);
		}
		ctx._currentCommandBuffer = {};

		ASSERT_MSG(_lastColorTexture.valid(), "There must be a color texture in the last Pass in order to display to Swapchain!");
		_hasCompiled = true;
	}

	// checks that all read textures are valid & have the correct layout
	static void ValidateReadImageLayouts(CTX& ctx, const std::vector<PassSource>& passes) {
		for (const PassSource& pass : passes) {
			for (const ReadSpec& read : pass.readOperations) {
				ASSERT(read.texture.valid());
				const VkImageLayout currentImageLayout = ctx.viewTexture(read.texture).getImageLayout();
				ASSERT_MSG(read.expectedLayout == currentImageLayout, "Texture '{}' does not have expected layout '{}' instead is in '{}'!", ctx.viewTexture(read.texture).getDebugName(), vkstring::VulkanImageLayoutToString(read.expectedLayout), vkstring::VulkanImageLayoutToString(currentImageLayout));
			}
		}
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		ASSERT_MSG(this->_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		ASSERT(!this->_compiledPasses.empty());

#ifdef DEBUG
		ValidateReadImageLayouts(*cmd._ctx, _sourcePasses);
#endif

		for (const PassCompiled& pass : _compiledPasses) {
			cmd._activePass = pass;

			// if we need to perform transitions
			if (!pass.preBarriers.empty()) {
				std::vector<VkImageMemoryBarrier2> barriers;
				barriers.reserve(pass.preBarriers.size());
				for (const CompiledBarrier& cb : pass.preBarriers) {
					// if we repeat ourselves in specifying we can just skip
					// if (cb.barrier.oldLayout == cb.barrier.newLayout) {
					// 	LOG_DEBUG("Unnecessary read operation for texture '{}' in pass '{}', is already in the image layout '{}'",
					// 		cmd._ctx->viewTexture(cb.textureHandle).getDebugName(),
					// 		pass.name,
					// 		vkstring::VulkanImageLayoutToString(cb.barrier.oldLayout));
					// 	continue;
					// }
					barriers.push_back(cb.barrier);
				}
				// write image barrier
				VkDependencyInfo dependencyInfo = {
						.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
						.pNext = nullptr,
						.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
						.pImageMemoryBarriers = barriers.data()
				};
				vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
				// update image layout for each texture object
				// needs to be updated before next pass in case it is used there
				for (const CompiledBarrier& cb : pass.preBarriers) {
					cmd._ctx->_texturePool.get(cb.textureHandle)->_vkCurrentImageLayout = cb.barrier.newLayout;
				}
			}
			// perform callback
			pass.executeCallback(cmd);
		}
		// todo: implement hints for this sort of thing, dont do it automatically
		ASSERT_MSG(_lastColorTexture.valid(), "Last writen color texture to be blit to swapchain is invalid!");
		cmd.cmdPrepareToSwapchainImpl(_lastColorTexture);
	}
}
