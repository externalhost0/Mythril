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
		passSource.executeCallback = callback;
		_graphRef._sourcePasses.push_back(std::move(passSource));
		// reset status incase user compiled and than adds another pass
		_graphRef._hasCompiled = false;
	}

	void RenderGraph::compile(const CTX& ctx) {
		// remove past compilations
		this->_compiledPasses.clear();

		for (const PassSource& source: this->_sourcePasses) {
			PassCompiled compiled;
			// first set some basic info
			compiled.name = source.name;
			compiled.type = source.type;
			compiled.executeCallback = source.executeCallback;
			// TODO: scuffed
			auto ex = ctx.getTexture(source.writeOperations.front().texture).getExtentAs2D();
			LOG_DEBUG("New pass extent2d is: {} x {} and sourced by: {}", ex.width, ex.height, ctx.getTexture(source.writeOperations.front().texture)._debugName);
			compiled.extent2D = ctx.getTexture(source.writeOperations.front().texture).getExtentAs2D();

			// STEP 1: PROCESS READ OPERATIONS
			for (const ReadSpec& readOperation: source.readOperations) {
				const AllocatedTexture& currentTexture = ctx.getTexture(readOperation.texture);
				VkImageMemoryBarrier2 barrier = vkinfo::CreateImageMemoryBarrier2(
						currentTexture._vkImage,
						currentTexture._vkFormat,
						currentTexture._vkCurrentImageLayout,
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						false);
				compiled.preBarriers.push_back({barrier, readOperation.texture});
			}
			// STEP 2: PROCESS WRITE OPERATIONS
			for (const WriteSpec& writeOperation: source.writeOperations) {
				const AllocatedTexture& currentTexture = ctx.getTexture(writeOperation.texture);
				if (currentTexture.isDepthAttachment()) {
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
					ColorAttachmentInfo colorInfo = {
							.imageView = currentTexture._vkImageView,
							.imageLayout = currentTexture._vkCurrentImageLayout,
							.imageFormat = currentTexture._vkFormat,

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
						const AllocatedTexture& resolveTexture = ctx.getTexture(writeOperation.resolveTexture.value());
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
			this->_compiledPasses.push_back(std::move(compiled));
		}
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

			cmd.cmdBeginRendering();
			pass.executeCallback(cmd);
			cmd.cmdEndRendering();
		}
		cmd.cmdPrepareToSwapchain(_lastColorTexture);
	}
}