//
// Created by Hayden Rivas on 10/11/25.
//

#include "CommandBuffer.h"
#include "vkinfo.h"
#include "vkenums.h"
#include "CTX.h"

#include "mythril/RenderGraphBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {
	GraphicsPassBuilder& GraphicsPassBuilder::write(const WriteSpec &spec) {
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

	// Validation helper: checks if all textures in writeSpec are the same resolution
	static void ValidateWriteOperationResolutions(const CTX& ctx, const PassSource& source, VkExtent2D referenceExtent2D) {
		const std::vector<WriteSpec>& writeOperations = source.writeOperations;
		for (size_t i = 0; i < writeOperations.size(); ++i) {
			const WriteSpec &writeOp = writeOperations[i];
			const AllocatedTexture &texture = ctx.viewTexture(writeOp.texture);
			const VkExtent2D extent = texture.getExtentAs2D();

			ASSERT_MSG(extent.width == referenceExtent2D.width && extent.height == referenceExtent2D.height,
					   "Pass '{}': Write attachment {} ('{}') has mismatched dimensions {}x{}, expected {}x{} (reference: '{}')",
					   source.name, i, texture.getDebugName(),
					   extent.width, extent.height, referenceExtent2D.width, referenceExtent2D.height,
					   ctx.viewTexture(source.writeOperations.front().texture).getDebugName());

			if (writeOp.resolveTexture.has_value()) {
				const AllocatedTexture &resolveTexture = ctx.viewTexture(writeOp.resolveTexture.value());
				const VkExtent2D resolveExtent = resolveTexture.getExtentAs2D();

				ASSERT_MSG(resolveExtent.width == referenceExtent2D.width &&
						   resolveExtent.height == referenceExtent2D.height,
						   "Pass '{}': Resolve attachment for '{}' has mismatched dimensions {}x{}, expected {}x{}",
						   source.name, texture.getDebugName(),
						   resolveExtent.width, resolveExtent.height,
						   referenceExtent2D.width, referenceExtent2D.height);
			}
		}
	}

	// Compile a read operation into a barrier
	// Note: oldLayout is set to UNDEFINED - it will be updated during execute() based on actual runtime state
	CompiledBarrier CompileReadOperation(CTX& ctx, const ReadSpec& readSpec) {
		const AllocatedTexture& currentTexture = ctx.viewTexture(readSpec.texture);
		ASSERT_MSG(currentTexture.getImage() != VK_NULL_HANDLE,
				   "Texture read operation could not be compiled for '{}'!",
				   currentTexture.getDebugName());

		VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
				currentTexture.getImage(),
				currentTexture.getFormat(),
				VK_IMAGE_LAYOUT_UNDEFINED,  // PLACEHOLDER
				readSpec.expectedLayout,
				false);
		return {barrier, readSpec.texture};
	}

	// Compile a write operation into a barrier
	// Note: oldLayout is set to UNDEFINED - it will be updated during execute() based on actual runtime state
	CompiledBarrier CompileWriteOperation(CTX& ctx, const WriteSpec& writeSpec, VkImageLayout newLayout) {
		const AllocatedTexture& currentTexture = ctx.viewTexture(writeSpec.texture);
		ASSERT_MSG(currentTexture.getImage() != VK_NULL_HANDLE,
				   "Texture write operation could not be compiled for '{}'!",
				   currentTexture.getDebugName());

		VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
				currentTexture.getImage(),
				currentTexture.getFormat(),
				VK_IMAGE_LAYOUT_UNDEFINED,  // PLACEHOLDER
				newLayout,
				false);

		return {barrier, writeSpec.texture};
	}

	void RenderGraph::compile(CTX& ctx) {
		this->_compiledPasses.clear();

		// BUILD PASS DESCRIPTIONS -> PIPELINE BARRIERS //
		for (const PassSource& source: this->_sourcePasses) {
			PassCompiled compiled;
			compiled.name = source.name;
			compiled.type = source.type;
			compiled.executeCallback = source.executeCallback;

			// STEP 1: PROCESS READ OPERATIONS
			for (const ReadSpec& readOperation : source.readOperations) {
				compiled.preBarriers.emplace_back(CompileReadOperation(ctx, readOperation));
			}

			// STEP 2: PROCESS WRITE OPERATIONS
			if (source.type == PassSource::Type::Graphics) {
				ASSERT_MSG(!source.writeOperations.empty(),
						   "Graphics Pass '{}' has no write operations, which is not allowed.",
						   source.name);

				VkExtent2D referenceExtent2D = ctx.viewTexture(source.writeOperations.front().texture).getExtentAs2D();
				compiled.extent2D = referenceExtent2D;

#ifdef DEBUG
				ValidateWriteOperationResolutions(ctx, source, referenceExtent2D);
#endif

				bool hasDepthAttachment = false;
				for (const WriteSpec& writeOperation: source.writeOperations) {
					const AllocatedTexture& currentTexture = ctx.viewTexture(writeOperation.texture);
					ASSERT_MSG(currentTexture._vkImage != VK_NULL_HANDLE,
							   "Pass '{}': Texture write operation references invalid vkImage for '{}'",
							   source.name, currentTexture.getDebugName());

					if (currentTexture.isDepthAttachment()) {
						ASSERT_MSG(!hasDepthAttachment,
								   "Pass '{}': Multiple depth attachments not allowed (found '{}')",
								   source.name, currentTexture.getDebugName());
						hasDepthAttachment = true;

						// Depth attachment info
						compiled.depthAttachment = DepthAttachmentInfo{
								.imageView = currentTexture._vkImageView,
								.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
								.imageFormat = currentTexture._vkFormat,
								.loadOp = toVulkan(writeOperation.loadOp),
								.storeOp = toVulkan(writeOperation.storeOp),
								.clearDepthStencil = writeOperation.clearValue.clearDepthStencil.getAsVkClearDepthStencilValue()
						};

						// create barrier for depth attachment
						constexpr VkImageLayout newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
						compiled.preBarriers.push_back(
							CompileWriteOperation(ctx, writeOperation, newLayout)
						);
					} else {
						// Color attachment info
						ColorAttachmentInfo colorInfo = {
							.imageView = currentTexture._vkImageView,
							.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							.imageFormat = currentTexture._vkFormat,
							// if resolve target is included, fill in the following steps
							.resolveImageView = VK_NULL_HANDLE,
							.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
							.loadOp = toVulkan(writeOperation.loadOp),
							.storeOp = toVulkan(writeOperation.storeOp),
							.clearColor = writeOperation.clearValue.clearColor.getAsVkClearColorValue(),
						};
						// Create barrier for color attachment
						constexpr VkImageLayout newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
						compiled.preBarriers.push_back(
							CompileWriteOperation(ctx, writeOperation, newLayout)
						);

						// Handle MSAA resolve
						if (writeOperation.resolveTexture.has_value()) {
							const AllocatedTexture& resolveTexture = ctx.viewTexture(writeOperation.resolveTexture.value());

							ASSERT_MSG(resolveTexture._vkImage != VK_NULL_HANDLE,
									   "Pass '{}': Resolve target '{}' for '{}' has invalid vkImage!",
									   source.name, resolveTexture.getDebugName(), currentTexture.getDebugName());
							ASSERT_MSG(currentTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT,
									   "Pass '{}': Resolve operation on non-multisampled texture '{}'!",
									   source.name, currentTexture.getDebugName());
							ASSERT_MSG(resolveTexture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
									   "Pass '{}': Resolve target '{}' is multisampled!",
									   source.name, resolveTexture.getDebugName());

							colorInfo.resolveImageView = resolveTexture._vkImageView;
							colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

							// create barrier for resolve target
							VkImageMemoryBarrier2 resolveBarrier = vkinfo::CreateImageMemoryBarrier2(
									resolveTexture._vkImage,
									resolveTexture._vkFormat,
									VK_IMAGE_LAYOUT_UNDEFINED,  // PLACEHOLDER
									VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									true);
							compiled.preBarriers.push_back({resolveBarrier, writeOperation.resolveTexture.value()});
						}

						compiled.colorAttachments.push_back(colorInfo);
					}
				}

				ASSERT_MSG(!compiled.colorAttachments.empty() || hasDepthAttachment,
						   "Pass '{}' was given no color or depth attachments!", source.name);
			}

			this->_compiledPasses.push_back(std::move(compiled));
		}

		// BUILD RENDER PIPELINES & other commands //
		// This runs the callbacks in a "dry run" mode to let them create pipelines
		for (const PassCompiled& pass : this->_compiledPasses) {
			CommandBuffer dryCmd;
			dryCmd._ctx = &ctx;
			dryCmd._activePass = pass;
			ctx._currentCommandBuffer = dryCmd;
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, something went horribly wrong!", pass.name);
			pass.executeCallback(dryCmd);
		}
		ctx._currentCommandBuffer = {};

		_hasCompiled = true;
	}

	// currently not used/broken
	static void ValidateReadImageLayouts(const CTX& ctx, const std::vector<PassSource>& passes) {
		for (const PassSource& pass : passes) {
			for (const ReadSpec& read : pass.readOperations) {
				ASSERT(read.texture.valid());
				const VkImageLayout currentImageLayout = ctx.viewTexture(read.texture).getImageLayout();
				ASSERT_MSG(read.expectedLayout == currentImageLayout,
						   "Pass '{}': Texture '{}' does not have expected layout '{}' instead is in '{}'!",
						   pass.name,
						   ctx.viewTexture(read.texture).getDebugName(),
						   vkstring::VulkanImageLayoutToString(read.expectedLayout),
						   vkstring::VulkanImageLayoutToString(currentImageLayout));
			}
		}
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(this->_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		if (this->_compiledPasses.empty()) {
			LOG_SYSTEM(LogType::Warning, "RenderGraph has no passes, execute will do nothing!");
			return;
		}
// #ifdef DEBUG
// 		ValidateReadImageLayouts(*cmd._ctx, _sourcePasses);
// #endif
		// execute each pass in order of added
		for (const PassCompiled& pass : _compiledPasses) {
			cmd._activePass = pass;
			// if barriers are required
			if (!pass.preBarriers.empty()) {
				std::vector<VkImageMemoryBarrier2> barriers;
				barriers.reserve(pass.preBarriers.size());

				for (const CompiledBarrier& compiled_bi : pass.preBarriers) {
					VkImageMemoryBarrier2 barrier = compiled_bi.barrier;
					const AllocatedTexture& tex = cmd._ctx->viewTexture(compiled_bi.textureHandle);
					// give old layout current layout at runtime
					barrier.oldLayout = tex.getImageLayout();
					// skip if already in target layout
					if (barrier.oldLayout == barrier.newLayout) {
						continue;
					}
					barriers.push_back(barrier);
				}
				// submit barriers if needed
				if (!barriers.empty()) {
					ASSERT(cmd._wrapper->_cmdBuf);
					VkDependencyInfo dependencyInfo = {
							.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
							.pNext = nullptr,
							.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
							.pImageMemoryBarriers = barriers.data()
					};
					vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
					// update tracked layouts after barriers execute
					for (const CompiledBarrier& cb : pass.preBarriers) {
						cmd._ctx->_texturePool.get(cb.textureHandle)->_vkCurrentImageLayout = cb.barrier.newLayout;
					}
				}
			}
			// execute the pass callback
			pass.executeCallback(cmd);
		}
	}
}