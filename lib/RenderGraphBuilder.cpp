//
// Created by Hayden Rivas on 10/11/25.
//

#include "mythril/CTX.h"
#include "CommandBuffer.h"
#include "mythril/vkenums.h"

#include "mythril/RenderGraphBuilder.h"

#include <array>
#include <memory_resource>
#include <cstdlib>
#include <fstream>
#include <queue>

#include "RenderGraphInternal.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {
	using namespace detail;
	RenderGraph::RenderGraph() = default;
	RenderGraph::~RenderGraph() = default;
	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	BasePassBuilder::BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type)
		: _rGraph(rGraph), _passSource(pName, type) {
		ASSERT(!_passSource.name.empty());
		// set default queue solely based on type, will change during compile
		_passSource.queue = [](PassDesc::Type type) {
			switch (type) {
				case PassDesc::Type::Graphics:
					return QueueAffinity::Graphics;
				case PassDesc::Type::Compute:
					return QueueAffinity::Graphics;
				case PassDesc::Type::Intermediate:
					return QueueAffinity::AsyncTransfer;
				default:
					return QueueAffinity::Graphics;
			}
		}(type);
	}

#ifdef DEBUG
	GraphicsPassBuilder::~GraphicsPassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_PASS(LogType::Warning, this->base._passSource, "graphics pass has no execution callback!");
	}
	ComputePassBuilder::~ComputePassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_PASS(LogType::Warning, this->base._passSource, "compute pass has no execution callback!");
	}
	RayTracingPassBuilder::~RayTracingPassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_PASS(LogType::Warning, this->base._passSource, "raytracing pass has no execution callback!");
	}
#endif

	IntermediateBuilder& IntermediateBuilder::blit(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, TextureAccess::TransferRead);
		add(base._passSource, dst, TextureAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture.getContext()->view(dst.texture.handle()).isSwapchainImage()) {
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

		add(base._passSource, src, TextureAccess::TransferRead);
		add(base._passSource, dst, TextureAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture.getContext()->view(dst.texture.handle()).isSwapchainImage()) {
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
	//IntermediateBuilder& IntermediateBuilder::generateMipmaps(const Texture& texture) {
	//	TextureHandle handle = texture.handle();
	//	auto oldCallback = base._passSource.executeCallback;
	//	base._passSource.executeCallback = [oldCallback, handle](CommandBuffer& cmd) {
	//		if (oldCallback)
	//			oldCallback(cmd);
	//		cmd.cmdGenerateMipmap(handle);
	//	};
	//	return *this;
	//}
	//IntermediateBuilder& IntermediateBuilder::dependency(Buffer& buffer, BufferAccess access) {
	//	add(base._passSource, buffer, access);
	//	return *this;
	//}
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
			ASSERT_PASS(data.data, *cmd._activePass, "IntermediateBuilder::update called with null data.");
			ASSERT_PASS(data.size > 0 && data.size <= 65536, *cmd._activePass, "IntermediateBuilder::update requires 0 < size <= 65'536 (64KB). Please call IntermediateBuilder::upload instead!");
			ASSERT_PASS(data.size % 4 == 0, *cmd._activePass, "IntermediateBuilder::update size must be a multiple of 4 bytes.");
			ASSERT_PASS(dstOffset % 4 == 0, *cmd._activePass, "IntermediateBuilder::update offset must be a multiple of 4 bytes.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_PASS(dstOffset + data.size <= buf._bufferSize, *cmd._activePass, "IntermediateBuilder::update: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_PASS(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, *cmd._activePass, "IntermediateBuilder::update: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);

			vkCmdUpdateBuffer(cmd._wrapper->_cmdBuf, buf._vkBuffer, dstOffset, data.size, data.data);
		};
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		BufferHandle handle = buffer.handle();
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		// fixme: if somehthing flakely inside an upload callback happens, this is the culprit
#ifdef DEBUG
		if (const size_t size = dataCb().size; size > 0 && size <= 65536)
			LOG_PASS(LogType::Suggestion, base._passSource, "upload into buffer '{}' is {} bytes (<= 64KB), use IntermediateBuilder::update instead for a faster operation!", buffer.getContext()->view(handle).getDebugName(), size);
#endif
		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle, dataCb = std::move(dataCb), dstOffset](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			if (cmd.isDrying())
				return;

			const UploadData data = dataCb();
			ASSERT_PASS(data.data, *cmd._activePass, "IntermediateBuilder::upload called with null data.");
			ASSERT_PASS(data.size > 0, *cmd._activePass, "IntermediateBuilder::upload size must be greater than 0.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_PASS(dstOffset + data.size <= buf._bufferSize, *cmd._activePass, "IntermediateBuilder::upload: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_PASS(
			        buf.isMapped() || (buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), *cmd._activePass, "IntermediateBuilder::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName
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

	void IntermediateBuilder::finish() const {
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
	static SubresourceRange ResolveSubresourceRange(const TextureDesc& texDesc, const AllocatedTexture& texture, const PassDesc& pass) {
		const uint32_t baseMip = texDesc.baseLevel.value_or(0);
		const uint32_t numMips = texDesc.numLevels.value_or(texDesc.baseLevel.has_value() ? 1 : texture.getNumMips());
		const uint32_t baseLayer = texDesc.baseLayer.value_or(0);
		const uint32_t numLayers = texDesc.numLayers.value_or(texDesc.baseLayer.has_value() ? 1 : texture.getNumLayers());

		ASSERT_PASS(baseMip < texture.getNumMips(), pass, "Texture '{}' base mip {} is outside the texture's {} mip levels", texture.getDebugName(), baseMip, texture.getNumMips());
		ASSERT_PASS(
		        numMips > 0 && baseMip + numMips <= texture.getNumMips(),
		        pass,
		        "Texture '{}' mip range [{}, {}) is outside the texture's {} mip levels",
		        texture.getDebugName(),
		        baseMip,
		        baseMip + numMips,
		        texture.getNumMips()
		);
		ASSERT_PASS(baseLayer < texture.getNumLayers(), pass, "Texture '{}' base layer {} is outside the texture's {} layers", texture.getDebugName(), baseLayer, texture.getNumLayers());
		ASSERT_PASS(
		        numLayers > 0 && baseLayer + numLayers <= texture.getNumLayers(),
		        pass,
		        "Texture '{}' layer range [{}, {}) is outside the texture's {} layers",
		        texture.getDebugName(),
		        baseLayer,
		        baseLayer + numLayers,
		        texture.getNumLayers()
		);

		return {baseMip, numMips, baseLayer, numLayers};
	}

	static VkImageView ResolveImageView(const TextureDesc& texDesc, const AllocatedTexture& allocatedTexture, const PassDesc& pass) {
		const bool needsCustomView = texDesc.baseLayer.has_value() || texDesc.baseLevel.has_value() || texDesc.numLayers.has_value() || texDesc.numLevels.has_value();
		if (!needsCustomView) {
			return allocatedTexture.getImageView();
		}

		const SubresourceRange range = ResolveSubresourceRange(texDesc, allocatedTexture, pass);
		VkImageViewType viewType = allocatedTexture.getViewType();
		if (range.numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
			viewType = VK_IMAGE_VIEW_TYPE_2D;
		}
		CTX* ctx = texDesc.texture.getContext();
		const TextureViewKey viewKey = ctx->createTextureView(texDesc.texture.handle(), {
		    .type = viewType,
		    .mipLevel = range.baseMip,
		    .numMipLevels = range.numMips,
		    .layer = range.baseLayer,
		    .numLayers = range.numLayers,
		});
		return ctx->getTextureView(texDesc.texture.handle(), viewKey);
	}

	static constexpr bool NeedsBarrier(VkImageLayout currentLayout, const vkutil::StageAccess& currentStageAccess, VkImageLayout desiredLayout, const vkutil::StageAccess& desiredStageAccess) {
		if (currentLayout != desiredLayout) return true;

		constexpr VkAccessFlags2 write_flags2 =
		        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

		// same layout: only read-after-read can skip the barrier
		const bool currentWrites = (currentStageAccess.access & write_flags2) != 0;
		const bool desiredWrites = (desiredStageAccess.access & write_flags2) != 0;
		return currentWrites || desiredWrites;
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
	static constexpr VkPipelineStageFlags2 GetPassShaderStages(PassDesc::Type passType) {
		switch (passType) {
			case PassDesc::Type::Graphics:
				return VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case PassDesc::Type::Compute:
				return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			case PassDesc::Type::Intermediate:
				return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			case PassDesc::Type::RayTracing:
				return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
		}
		return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	}
	static constexpr VkImageLayout GetTextureLayout(TextureAccess access) {
		switch (access) {
			case TextureAccess::Sampled:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureAccess::StorageRead:
			case TextureAccess::StorageWrite:
			case TextureAccess::StorageReadWrite:
				return VK_IMAGE_LAYOUT_GENERAL;
			case TextureAccess::TransferRead:
				return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case TextureAccess::TransferWrite:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		}
		ASSERT_MSG(false, "Unsupported TextureAccess value.");
	}
	static vkutil::StageAccess GetTextureStageAccess(TextureAccess access, PassDesc::Type passType) {
		const VkPipelineStageFlags2 shaderStages = GetPassShaderStages(passType);
		// SHADER_READ covers bindless handles that end up sampled rather than loaded
		switch (access) {
			case TextureAccess::Sampled:
			case TextureAccess::StorageRead:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_READ_BIT};
			case TextureAccess::StorageWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_WRITE_BIT};
			case TextureAccess::StorageReadWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
			case TextureAccess::TransferRead:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT};
			case TextureAccess::TransferWrite:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT};
		}
		ASSERT_MSG(false, "Unsupported TextureAccess value.");
	}
	static vkutil::StageAccess GetBufferStageAccess(BufferAccess access, PassDesc::Type passType) {
		const VkPipelineStageFlags2 shaderStages = GetPassShaderStages(passType);
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

	void RenderGraph::processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, const StageAccess& dstMask, CompiledPass& outPass, const PassDesc& pass) {
		const AllocatedTexture& texture = texDesc.texture.getContext()->view(texDesc.texture.handle());
		const bool isSwapchain = texture.isSwapchainImage();
		const SubresourceRange& range = ResolveSubresourceRange(texDesc, texture, pass);
		outPass.imageBarriers.push_back({.handle = texDesc.texture.handle(), .range = range, .dstLayout = desiredLayout, .dstMask = dstMask, .isSwapchain = isSwapchain});
	}

	void RenderGraph::processPassResources(const PassDesc& passDesc, CompiledPass& outPass) {
		// we expect to use a max of this
		outPass.imageBarriers.reserve(passDesc.textureDependencyOperations.size() + passDesc.attachmentOperations.size());
		outPass.bufferBarriers.reserve(passDesc.bufferDependencyOperations.size());
		for (const TextureDependencyDesc& dependency_desc: passDesc.textureDependencyOperations) {
			processResourceAccess(dependency_desc.texDesc, GetTextureLayout(dependency_desc.access), GetTextureStageAccess(dependency_desc.access, passDesc.type), outPass, passDesc);
		}
		for (const AttachmentDesc& attachment_desc: passDesc.attachmentOperations) {
			const AllocatedTexture& texture = attachment_desc.texDesc.texture.getContext()->view(attachment_desc.texDesc.texture.handle());
			const VkImageLayout layout = texture.isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			processResourceAccess(attachment_desc.texDesc, layout, vkutil::GetPipelineStageAccess(layout), outPass, passDesc);

			if (attachment_desc.resolveTexDesc.has_value()) {
				// resolves always write in color attachment output, even for depth/stencil
				constexpr StageAccess resolveMask = {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
				processResourceAccess(attachment_desc.resolveTexDesc.value(), layout, resolveMask, outPass, passDesc);
			}
		}
		for (const BufferDependencyDesc& dependency_desc: passDesc.bufferDependencyOperations) {
			outPass.bufferBarriers.push_back({.handle = dependency_desc.buffer.handle(), .dstMask = GetBufferStageAccess(dependency_desc.access, passDesc.type)});
		}
	}

	static std::optional<VkMemoryBarrier2> BuildInOrderBarrier(const CompiledPass& pass) {
		constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		StageAccess src{};
		StageAccess dst{};
		auto accumulate = [&](const StageAccess& mask) {
			dst.stage |= mask.stage;
			dst.access |= mask.access;
			if (mask.access & kWriteAccess) {
				src.stage |= mask.stage;
				src.access |= mask.access & kWriteAccess;
			}
		};
		for (const CompiledImageBarrier& barrier: pass.imageBarriers)
			accumulate(barrier.dstMask);
		for (const CompiledBufferBarrier& barrier: pass.bufferBarriers)
			accumulate(barrier.dstMask);
		if (src.access == 0)
			return std::nullopt;
		return VkMemoryBarrier2{
		    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		    .srcStageMask = src.stage,
		    .srcAccessMask = src.access,
		    .dstStageMask = dst.stage,
		    .dstAccessMask = dst.access,
		};
	}

	void RenderGraph::processAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) {
		if (pass_desc.attachmentOperations.empty())
			return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc: pass_desc.attachmentOperations) {
			const TextureDesc& texDesc = attachment_desc.texDesc;
			const AllocatedTexture& allocatedTexture = attachment_desc.texDesc.texture.getContext()->view(attachment_desc.texDesc.texture.handle());

			const bool isDepthAttachment = vkutil::IsFormatDepth(allocatedTexture.getFormat());
			if (isDepthAttachment) {
				ASSERT_PASS(!hasDepthAttachment, pass_desc, "Multiple depth attachments not allowed (found '{}' to be a second depth attachment)", allocatedTexture.getDebugName());
				hasDepthAttachment = true;
			}
			VkImageView imageView = ResolveImageView(texDesc, allocatedTexture, pass_desc);

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
				const AllocatedTexture resolve_texture = resolve_desc.texture.getContext()->view(resolve_desc.texture.handle());
				ASSERT_PASS(
				        resolve_texture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
				        pass_desc,
				        "Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
				        resolve_texture.getDebugName()
				);
				ASSERT_PASS(
				        allocatedTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT, pass_desc, "Resolve operation on non-multisampled texture '{}'!", allocatedTexture.getDebugName()
				);
				ASSERT_PASS(
				        resolve_texture.getFormat() == allocatedTexture.getFormat(),
				        pass_desc,
				        "Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
				        allocatedTexture.getDebugName(),
				        vkstring::VulkanFormatToString(allocatedTexture.getFormat()),
				        resolve_texture.getDebugName(),
				        vkstring::VulkanFormatToString(resolve_texture.getFormat())
				);
				if (attachment_desc.storeOp == StoreOp::STORE)
					LOG_PASS(
					        LogType::Suggestion,
					        pass_desc,
					        "Attachment of texture '{}' has StoreOp::STORE and resolves into '{}', can be replaced with StoreOp::DONT_CARE.",
					        allocatedTexture.getDebugName(),
					        resolve_texture.getDebugName()
					);

				// set the fields we left empty previously
				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveImageView = ResolveImageView(resolve_desc, resolve_texture, pass_desc);
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
				LOG_PASS(LogType::Warning, pass_desc, "You have attachments of different dimensions, this is allowed but experimental.");
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
			dryCmd._activePass = &pass;
			dryCmd._viewMask = pass.viewMask;
			rCtx._currentCommandBuffer = dryCmd;
			ASSERT_PASS(pass.executeCallback != nullptr, pass, "doesn't have an execute callback, something went horribly wrong!");
			pass.executeCallback(dryCmd);
		}
		rCtx._currentCommandBuffer = saved;
	}

	bool isWrite(TextureAccess access) {
		return access == TextureAccess::StorageWrite || access == TextureAccess::StorageReadWrite || access == TextureAccess::TransferWrite;
	}
	bool isWrite(BufferAccess access) {
		return access == BufferAccess::ShaderReadWrite || access == BufferAccess::ShaderWrite || access == BufferAccess::TransferWrite;
	}


	// unweighted, directed
	// fixed vertex amount
	template<size_t Vertices>
	struct AdjacencyMatrix {
		void addEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return;
			size_t bitIndex = (y * Vertices) + x;
			data.set(bitIndex);
		}
		void removeEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return;
			size_t bitIndex = (y * Vertices) + x;
			data.reset(bitIndex);
		}
		bool hasEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return false;
			return data.test((y * Vertices) + x);
		}
		std::bitset<Vertices * Vertices> data;
	};

	struct DirectedGraph {
		size_t nodeCount;
		// indicies represent original pass index
		// predecessors[2] = {0, 1} represents the third pass that must wait until first and second run
		// will always be same size vector as the # of passes
		std::vector<std::unordered_set<uint32_t>> predecessors;
		std::vector<std::vector<uint32_t>> successors;
		std::vector<uint32_t> inDegree;

		explicit DirectedGraph(uint32_t vertexCount) : nodeCount(vertexCount), predecessors(vertexCount), successors(vertexCount), inDegree(vertexCount) {}

		void addEdge(uint32_t from, uint32_t to) {
			if (from == to) return;
			predecessors.at(to).insert(from);
		}

		std::optional<std::vector<uint32_t>> topologicalSort() const {
			std::vector<uint32_t> local_in_degrees = inDegree;
			std::queue<uint32_t> ready;
			std::vector<uint32_t> sorted_order;
			sorted_order.reserve(this->nodeCount);

			// get root nodes, aka a node with no degrees incoming
			for (uint32_t i = 0; i < nodeCount; ++i) {
				if (local_in_degrees[i] == 0)
					ready.push(i);
			}
			// process from root nodes
			while (!ready.empty()) {
				uint32_t node = ready.front(); ready.pop();
				sorted_order.push_back(node);
				for (uint32_t successor : successors[node]) {
					if (--local_in_degrees[successor] == 0)
						ready.push(successor);
				}
			}
			// if size is not the same than cycles exist, thus cant be topo sorted
			if (sorted_order.size() != nodeCount) return std::nullopt;
			return sorted_order;
		}
	};

	DirectedGraph build_directed_graph(const std::vector<PassDesc>& pass_descs) {
		const size_t pass_desc_count = pass_descs.size();

		auto collectTexAccess = [](const PassDesc& desc) {
			std::vector<std::pair<TextureHandle, bool>> result;
			result.reserve(desc.textureDependencyOperations.size() + desc.attachmentOperations.size());
			for (const auto& texDep : desc.textureDependencyOperations) {
				result.emplace_back(texDep.texDesc.texture.handle(), isWrite(texDep.access));
			}
			for (const auto& attachDep : desc.attachmentOperations) {
				// an attachment is always written to
				result.emplace_back(attachDep.texDesc.texture.handle(), true);
				if (attachDep.resolveTexDesc.has_value())
					result.emplace_back(attachDep.resolveTexDesc->texture.handle(), true);
			}
			return result;
		};
		auto collectBufAccess = [](const PassDesc& desc) {
			std::vector<std::pair<BufferHandle, bool>> result;
			result.reserve(desc.bufferDependencyOperations.size());
			for (const auto& bufDep : desc.bufferDependencyOperations) {
				result.emplace_back(bufDep.buffer.handle(), isWrite(bufDep.access));
			}
			return result;
		};

		// build our tracking vars
		// where a handle represents a resource, the integar represents its pass_id
		std::unordered_map<TextureHandle, uint32_t> lastTexWriter;
		std::unordered_map<TextureHandle, std::vector<uint32_t>> lastTexReaders;

		std::unordered_map<BufferHandle, uint32_t> lastBufWriter;
		std::unordered_map<BufferHandle, std::vector<uint32_t>> lastBufReaders;

		DirectedGraph result(pass_desc_count);
		// first collect all edges from the graph sequentially
		for (int i = 0; i < pass_desc_count; i++) {
			const PassDesc& desc = pass_descs[i];
			// h = handle
			// go through textures
			for (const auto& [h, isWrite] : collectTexAccess(desc)) {
				// if the resource isnt added already
				if (auto it = lastTexWriter.find(h); it != lastTexWriter.end())
					result.addEdge(it->second, i);

				if (isWrite) {
					// WAW
					for (const uint32_t reader : lastTexReaders[h])
						result.addEdge(reader, i);
					lastTexWriter[h] = i;
					lastTexReaders[h].clear();
				} else {
					// WAR
					lastTexReaders[h].push_back(i);
				}
			}
			// go through buffers
			for (const auto& [h, isWrite] : collectBufAccess(desc)) {
				if (auto it = lastBufWriter.find(h); it != lastBufWriter.end())
					result.addEdge(it->second, i);
				if (isWrite) {
					for (const uint32_t reader : lastBufReaders[h])
						result.addEdge(reader, i);
					lastBufWriter[h] = i;
					lastBufReaders[h].clear();
				} else {
					lastBufReaders[h].push_back(i);
				}
			}
		}
		return result;
	}

	void report_cycle_and_assert(const DirectedGraph& graph, const std::vector<PassDesc>& pass_descs) {
		const size_t node_count = graph.nodeCount;
		// copy this
		std::vector<uint32_t> local_in_degree = graph.inDegree;
		std::queue<uint32_t> ready;
		std::vector resolved(node_count, false);
		size_t resolved_count = 0;

		for (uint32_t i = 0; i < node_count; ++i) {
			if (local_in_degree[i] == 0) {
				ready.push(i);
			}
		}
		while (!ready.empty()) {
			const uint32_t node = ready.front();
			ready.pop();
			resolved[node] = true;
			resolved_count++;

			for (uint32_t succ : graph.successors[node]) {
				if (--local_in_degree[succ] == 0) {
					ready.push(succ);
				}
			}
		}
		// this should never happen :p
		if (resolved_count == node_count) return;
		// actually perform reporting now
		std::string report_string;
		for (uint32_t i = 0; i < node_count; ++i) {
			if (!resolved[i]) {
				report_string += fmt::format("\n- '{}' is stuck waiting on:", pass_descs[i].name);
				for (uint32_t pred : graph.predecessors[i]) {
					if (!resolved[pred]) {
						report_string += fmt::format(" '{}'", pass_descs[pred].name);
					}
				}
			}
		}
		ASSERT_MSG(false, "RenderGraph has a cycle ({} passes unresolved): {}", (node_count - resolved_count), report_string);
	}

	std::vector<uint32_t> cull_unused_passes(
		const std::vector<uint32_t>& topo_sorted_order,
		const std::vector<std::unordered_set<uint32_t>>& predecessors,
		const std::vector<PassDesc>& pass_descs) {

		const size_t node_count = topo_sorted_order.size();
		// now that sortedOrder is topologically sorted, we can perform culling from the swapchain pass
		// todo: this only walks back from a swapchain image, headless will mean everything is culled
		std::unordered_set<uint32_t> sinkPasses;
		for (uint32_t i = 0; i < node_count; i++) {
			const PassDesc& pass_desc = pass_descs[i];
			for (const auto& attachDesc : pass_desc.attachmentOperations) {
				if (attachDesc.texDesc.texture.getContext()->isSwapchainTexture(attachDesc.texDesc.texture.handle())) {
					sinkPasses.insert(i);
					break;
				}
			}
			// verify that it is a write to swapchain not a read for some reason
			for (const auto& texDesc : pass_desc.textureDependencyOperations) {
				if (texDesc.texDesc.texture.getContext()->isSwapchainTexture(texDesc.texDesc.texture.handle()) && isWrite(texDesc.access)) {
					sinkPasses.insert(i);
					break;
				}
			}
		}
		// now reverse BFS
		std::unordered_set<uint32_t> reachablePasses;
		std::queue<uint32_t> worklist;
		for (uint32_t sink : sinkPasses) {
			worklist.push(sink);
			reachablePasses.insert(sink);
		}
		while (!worklist.empty()) {
			const uint32_t node = worklist.front(); worklist.pop();
			for (uint32_t pred : predecessors[node]) {
				if (reachablePasses.insert(pred).second)
					worklist.push(pred);
			}
		}
		// filter sortedOrder while preserving order
		std::vector<uint32_t> result;
		result.reserve(reachablePasses.size());
		for (uint32_t passIdx : topo_sorted_order) {
			if (reachablePasses.contains(passIdx))
				result.push_back(passIdx);
		}
		return result;
	}

	ExecutionSchedule calculate_execution_schedule(const std::vector<uint32_t>& culled_order, const std::vector<std::unordered_set<uint32_t>>& predecessors) {
		ExecutionSchedule result;
		uint32_t max_depth = 0;
		// find the depth for every active (culled) pass
		for (const uint32_t pass_idx : culled_order) {
			uint32_t current_depth = 0;
			for (uint32_t pred : predecessors[pass_idx]) {
				// only get predecessors that arent culled
				if (auto it = result.pass_depths.find(pred); it != result.pass_depths.end()) {
					current_depth = std::max(current_depth, it->second + 1);
				}
			}
			result.pass_depths[pass_idx] = current_depth;
			// update max if necessary
			max_depth = std::max(max_depth, current_depth);
		}
		// group passes into layers
		result.layers.resize(max_depth + 1);
		for (const uint32_t pass_idx : culled_order) {
			const uint32_t depth = result.pass_depths[pass_idx];
			result.layers[depth].push_back(pass_idx);
		}
		return result;
	}

	void RenderGraph::compile(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		this->_compiledPasses.clear();
		std::erase_if(_resourceTrackers, [&rCtx](const auto& entry) { return rCtx._texturePool.get(entry.first) == nullptr; });
		this->_bufferTrackers.clear();

		std::vector<uint32_t> culled_order;
		ExecutionSchedule schedule;
		{
			const DirectedGraph directed_graph = build_directed_graph(this->_passDescriptions);

			// yeah i know this is doing more work then necessary but its way better to understand
			const std::vector<std::unordered_set<uint32_t>>& passPredecessors = directed_graph.predecessors;
			const std::optional<std::vector<uint32_t>>& sortedOrderOptional = directed_graph.topologicalSort();
			if (!sortedOrderOptional.has_value()) {
				// who cares about the recalculation overhead
				report_cycle_and_assert(directed_graph, this->_passDescriptions);
			}
			culled_order = cull_unused_passes(sortedOrderOptional.value(), passPredecessors, _passDescriptions);

			schedule = calculate_execution_schedule(culled_order, passPredecessors);
			this->_schedule = schedule;
		}

		// map from passDescription to _compiledPasses, reversed basically
		std::unordered_map<uint32_t, uint32_t> passToCompiled;
		passToCompiled.reserve(culled_order.size());
		for (uint32_t i = 0; i < static_cast<uint32_t>(culled_order.size()); ++i)
			passToCompiled[culled_order[i]] = i;

		// assign and warn queues for each pass
		const bool canUseAC = rCtx._immAsyncCompute != nullptr;
		const bool canUseAT = rCtx._immAsyncTransfer != nullptr;
		auto sanitizeQueue = [canUseAC, canUseAT](const QueueAffinity requested_queue, const PassDesc& pass) {
			// demote passes that request a queue but we dont have access to
			if (requested_queue == QueueAffinity::AsyncCompute && !canUseAC) {
				LOG_PASS(LogType::Warning, pass, "demoted to graphics: No async compute family available.");
				return QueueAffinity::AsyncTransfer;
			}
			if (requested_queue == QueueAffinity::AsyncTransfer && !canUseAT) {
				if (canUseAC) {
					LOG_PASS(LogType::Info, pass, "routed to async compute: No dedicated transfer family.");
					return QueueAffinity::AsyncCompute;
				}
				LOG_PASS(LogType::Warning, pass, "demoted to graphics: No async family available.");
				return QueueAffinity::Graphics;
			}
			return requested_queue;
		};

		// compile works in this order
		this->_compiledPasses.reserve(culled_order.size());
		for (const uint32_t pass_idx : culled_order) {
			const PassDesc& pass_desc = this->_passDescriptions[pass_idx];
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.passIndex = pass_idx;
			compiled_pass.queue = sanitizeQueue(pass_desc.queue, pass_desc);
			compiled_pass.type = pass_desc.type;
			compiled_pass.executeCallback = pass_desc.executeCallback;
			compiled_pass.layerCount = pass_desc.layerCount;
			compiled_pass.viewMask = pass_desc.viewMask;
			compiled_pass.conditionCallback = pass_desc.conditionCallback;

			// do not worry about the return value,
			// every type of process should run for every pass
			processPassResources(pass_desc, compiled_pass);
			processAttachments(rCtx, pass_desc, compiled_pass);
			if (pass_desc.executeInOrder)
				compiled_pass.inOrderBarrier = BuildInOrderBarrier(compiled_pass);

			this->_compiledPasses.push_back(std::move(compiled_pass));
		}


		// build _layerByQueue from post cleaned _compiledPassses
		// requires that correct queues have already been selected
		this->_layerByQueue.assign(schedule.layers.size(), {});
		for (size_t depth = 0; depth < schedule.layers.size(); ++depth) {
			for (const uint32_t pass_idx : schedule.layers[depth]) {
				const uint32_t compiled_idx = passToCompiled[pass_idx];
				const auto q = static_cast<size_t>(_compiledPasses[compiled_idx].queue);
				this->_layerByQueue[depth][q].push_back(compiled_idx);
			}
		}
		performDryRun(rCtx);
		this->_hasCompiled = true;
		this->_compiledEpoch = rCtx._resourceEpoch;
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
			tracked.texture->getContext()->resizeTexture(tracked.texture->handle(), nextDimensions);
		}
	}

	void RenderGraph::PerformBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass) {
		if (compiledPass.imageBarriers.empty() && compiledPass.bufferBarriers.empty())
			return;
		ASSERT(cmd._wrapper->_cmdBuf);

		// per-pass scratch lives on the stack and only spills to the heap for unusually large passes
		std::array<std::byte, 8192> scratch;
		std::pmr::monotonic_buffer_resource arena{scratch.data(), scratch.size()};
		std::pmr::vector<VkImageMemoryBarrier2> vkImageBarriers{&arena};
		vkImageBarriers.reserve(compiledPass.imageBarriers.size());
		std::pmr::vector<VkBufferMemoryBarrier2> vkBufferBarriers{&arena};
		vkBufferBarriers.reserve(compiledPass.bufferBarriers.size());
		std::pmr::vector<SubresourceMap<StageAccess>::Entry> mergedReads{&arena};
		for (const CompiledImageBarrier& req: compiledPass.imageBarriers) {
			// we have to update the image associated if its a swapchain image
			// as it changes every frame
			const TextureHandle activeHandle = req.isSwapchain ? cmd._ctx->getCurrentSwapchainTexHandle() : req.handle;
			AllocatedTexture& activeTexture = cmd._ctx->access(activeHandle);
			auto [it, inserted] = _resourceTrackers.try_emplace(activeHandle, TextureAccessCache{SubresourceMap<StageAccess>{activeTexture.getNumMips(), activeTexture.getNumLayers()}});
			TextureAccessCache& cache = it->second;
			if (inserted || cache.layoutVersion != activeTexture.getLayoutVersion()) {
				// something outside the graph touched the image, so its last access is unknown
				cache.masks.set(cache.masks.wholeRange(), {.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_MEMORY_WRITE_BIT});
			}
			activeTexture._recordedLayout.forEachOverlapping(req.range, [&](const SubresourceRange& layoutRange, VkImageLayout currentLayout) {
				cache.masks.forEachOverlapping(layoutRange, [&](const SubresourceRange& range, const StageAccess& currentMask) {
					if (!NeedsBarrier(currentLayout, currentMask, req.dstLayout, req.dstMask)) {
						// a later write must wait on every reader since the last barrier
						mergedReads.push_back({range, {.stage = currentMask.stage | req.dstMask.stage, .access = currentMask.access | req.dstMask.access}});
						return;
					}
					vkImageBarriers.push_back(
					        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					         .srcStageMask = currentMask.stage,
					         .srcAccessMask = currentMask.access,
					         .dstStageMask = req.dstMask.stage,
					         .dstAccessMask = req.dstMask.access,
					         .oldLayout = currentLayout,
					         .newLayout = req.dstLayout,
					         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					         .image = activeTexture.getImage(),
					         .subresourceRange = MakeVkRange(activeTexture.getFormat(), range)}
					);
				});
			});
			cache.masks.set(req.range, req.dstMask);
			for (const auto& [range, merged]: mergedReads) {
				cache.masks.set(range, merged);
			}
			mergedReads.clear();
			activeTexture.setLayout(req.range, req.dstLayout);
			cache.layoutVersion = activeTexture.getLayoutVersion();
		}
		for (const CompiledBufferBarrier& req: compiledPass.bufferBarriers) {
			const AllocatedBuffer& buffer = cmd._ctx->view(req.handle);
			vkutil::StageAccess& tracked = _bufferTrackers[req.handle];
			const vkutil::StageAccess currentState = tracked;
			if (NeedsBufferBarrier(currentState, req.dstMask)) {
				vkBufferBarriers.push_back({
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
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
			tracked = req.dstMask;
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

	// will be the only execute but left for now
	void RenderGraph::executeEXT(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(_hasCompiled, "A RenderGraph must be compiled before it can be executed!");

		ASSERT_MSG(!rCtx.isHeadless(), "headless is currently not supported");

		// auto-recompile if any resource topology changed since last compile
		// (texture resize, swapchain recreate/destroy).
		if (_compiledEpoch != rCtx._resourceEpoch) {
			LOG_SYSTEM(LogType::Info, "Now performing recompile on rendergraph...");
			compile(rCtx);
		}

		CommandBuffer& cmd = rCtx.acquireCommand(CommandBuffer::Type::Graphics);
		this->execute(cmd);
		rCtx.submitCommand(cmd);
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
			if (pass.conditionCallback && !pass.conditionCallback())
				continue;
			const bool isGraphics = pass.type == PassDesc::Type::Graphics;
			// perform batched vkCmdPipelineBarrier
			PerformBarrierTransitions(cmd, pass);
			// keep GPU zones per pass, timestamps inside a render pass split Metal encoders on MoltenVK
			MYTH_PROFILER_GPU_ZONE(cmd._ctx, pass.name.c_str(), cmd._wrapper->_cmdBuf);
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
			cmd._activePass = &pass;
			cmd._hasDispatchedInPass = false;
			// we already checked if it has an execute callback so it should be guaranteed
			if (isGraphics) cmd.cmdBeginRenderingEXT(pass.layerCount, pass.viewMask);
			pass.executeCallback(cmd);
			if (isGraphics) cmd.cmdEndRenderingEXT();
		}
		if (!cmd._ctx->isHeadless()) {
			// fixme: make this cleaner im so lazy right now
			cmd.cmdTransitionLayout(cmd._ctx->getCurrentSwapchainTexHandle(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		}
	}
} // namespace mythril
