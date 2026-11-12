//
// Created by Hayden Rivas on 10/11/25.
//

#include "mythril/CTX.h"
#include "CommandBuffer.h"
#include "mythril/vkenums.h"

#include "mythril/RenderGraphBuilder.h"

#include <array>
#include <memory_resource>
#include <numeric>
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

	static uint32_t IdOf(const GraphTexture texture) {
		return GraphTextureAccess::index(texture);
	}
	static const GraphResource& ResourceOf(const std::vector<GraphResource>& textures, const GraphTexture texture) {
		ASSERT_MSG(texture.valid() && IdOf(texture) < textures.size(), "GraphTexture does not belong to this RenderGraph.");
		return textures[IdOf(texture)];
	}
	static bool IsBackBuffer(const std::vector<GraphResource>& textures, const GraphTexture texture) {
		return std::holds_alternative<SwapchainTexture>(ResourceOf(textures, texture).source);
	}
	static TextureHandle ResolveHandle(const std::vector<GraphResource>& textures, const CTX& rCtx, const GraphTexture texture) {
		const GraphResource& resource = ResourceOf(textures, texture);
		if (const auto* imported = std::get_if<ImportedTexture>(&resource.source))
			return imported->handle;
		if (const auto* declared = std::get_if<DeclaredTexture>(&resource.source)) {
			ASSERT_MSG(declared->texture.valid(), "Graph texture '{}' has no image, no surviving pass uses it.", declared->spec.debugName);
			return declared->texture.handle();
		}
		return rCtx.getCurrentSwapchainTexHandle();
	}

	ResolvedTexture RenderGraph::resolve(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, const GraphTexture texture) const {
		const TextureHandle handle = ResolveHandle(_textures, rCtx, texture);
#ifdef DEBUG
		ASSERT_PASS(std::ranges::binary_search(member.sortedDeclaredTextures, IdOf(texture), {}, IdOf), pass, "'{}' reads texture '{}' without declaring it.", member.name, rCtx.view(handle).getDebugName());
#endif
		return {handle, rCtx.getTextureDimensions(handle)};
	}

	GraphTexture RenderGraph::importTexture(const Texture& texture) {
		ASSERT_MSG(texture.valid(), "RenderGraph::importTexture called with an invalid texture.");
		ASSERT_MSG(!texture.getContext()->isSwapchainTexture(texture.handle()), "Swapchain images are not imported, use RenderGraph::backBuffer().");
		for (uint32_t i = 0; i < _textures.size(); ++i) {
			if (const auto* imported = std::get_if<ImportedTexture>(&_textures[i].source); imported && imported->handle == texture.handle())
				return GraphTexture{i};
		}
		_textures.push_back({ImportedTexture{texture.handle()}});
		return GraphTexture{static_cast<uint32_t>(_textures.size() - 1)};
	}

	GraphTexture RenderGraph::createTexture(GraphTextureSpec spec) {
		ASSERT_MSG(spec.format != VK_FORMAT_UNDEFINED, "RenderGraph::createTexture: '{}' needs a format.", spec.debugName);
		ASSERT_MSG(spec.size.width > 0 && spec.size.height > 0 && spec.size.depth > 0, "RenderGraph::createTexture: '{}' has a zero dimension.", spec.debugName);
		ASSERT_MSG(spec.numMipLevels > 0 && spec.numLayers > 0, "RenderGraph::createTexture: '{}' needs at least one mip level and layer.", spec.debugName);
		_textures.push_back({DeclaredTexture{.spec = std::move(spec)}});
		_hasCompiled = false;
		return GraphTexture{static_cast<uint32_t>(_textures.size() - 1)};
	}

	void RenderGraph::exportTexture(const GraphTexture texture) {
		ResourceOf(_textures, texture);
		auto* declared = std::get_if<DeclaredTexture>(&_textures[IdOf(texture)].source);
		ASSERT_MSG(declared, "RenderGraph::exportTexture only takes textures from RenderGraph::createTexture, imports are already reachable outside the graph.");
		declared->exported = true;
		_hasCompiled = false;
	}

	TextureHandle RenderGraph::exported(const GraphTexture texture) const {
		const auto* declared = std::get_if<DeclaredTexture>(&ResourceOf(_textures, texture).source);
		ASSERT_MSG(declared && declared->exported, "RenderGraph::exported called on a texture that was never passed to RenderGraph::exportTexture.");
		ASSERT_MSG(declared->texture.valid(), "RenderGraph::exported: '{}' has no image until the graph compiles.", declared->spec.debugName);
		return declared->texture.handle();
	}

	GraphTexture RenderGraph::backBuffer() {
		for (uint32_t i = 0; i < _textures.size(); ++i) {
			if (std::holds_alternative<SwapchainTexture>(_textures[i].source))
				return GraphTexture{i};
		}
		_textures.push_back({SwapchainTexture{}});
		return GraphTexture{static_cast<uint32_t>(_textures.size() - 1)};
	}

	BasePassBuilder::BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type)
		: _rGraph(rGraph), _passSource(pName, type) {
		ASSERT(!_passSource.name.empty());
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
		add(base._passSource, src, TextureAccess::TransferRead);
		add(base._passSource, dst, TextureAccess::TransferWrite);
		base._passSource.intermediateOps.emplace_back(BlitOp{src.texture, dst.texture});
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::copy(TextureDesc src, TextureDesc dst) {
		add(base._passSource, src, TextureAccess::TransferRead);
		add(base._passSource, dst, TextureAccess::TransferWrite);
		base._passSource.intermediateOps.emplace_back(CopyOp{src.texture, dst.texture});
		return *this;
	}

	IntermediateBuilder& IntermediateBuilder::update(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		add(base._passSource, buffer, BufferAccess::TransferWrite);
		base._passSource.intermediateOps.emplace_back(UpdateOp{buffer.handle(), dstOffset, std::move(dataCb)});
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		// fixme: if somehthing flakely inside an upload callback happens, this is the culprit
#ifdef DEBUG
		if (const size_t size = dataCb().size; size > 0 && size <= 65536)
			LOG_PASS(LogType::Suggestion, base._passSource, "upload into buffer '{}' is {} bytes (<= 64KB), use IntermediateBuilder::update instead for a faster operation!", buffer.getContext()->view(buffer.handle()).getDebugName(), size);
#endif
		base._passSource.intermediateOps.emplace_back(UploadOp{buffer.handle(), dstOffset, std::move(dataCb)});
		return *this;
	}

	void IntermediateBuilder::finish() const {
		ASSERT_PASS(!base._passSource.intermediateOps.empty(), base._passSource, "intermediate pass has no operations!");
		this->base._rGraph._passDescriptions.push_back(base._passSource);
		this->base._rGraph._hasCompiled = false;
	}

	void RenderGraph::RecordIntermediateOps(CommandBuffer& cmd, const CompiledPass& pass) const {
		for (const IntermediateOp& op: pass.intermediateOps) {
			if (const auto* copy = std::get_if<CopyOp>(&op)) {
				if (IsBackBuffer(_textures, copy->dst))
					cmd.cmdCopyImageToSwapchain(ResolveHandle(_textures, *cmd._ctx, copy->src));
				else
					cmd.cmdCopyImage(ResolveHandle(_textures, *cmd._ctx, copy->src), ResolveHandle(_textures, *cmd._ctx, copy->dst));
			} else if (const auto* blit = std::get_if<BlitOp>(&op)) {
				if (IsBackBuffer(_textures, blit->dst))
					cmd.cmdBlitImageToSwapchain(ResolveHandle(_textures, *cmd._ctx, blit->src));
				else
					cmd.cmdBlitImage(ResolveHandle(_textures, *cmd._ctx, blit->src), ResolveHandle(_textures, *cmd._ctx, blit->dst));
			} else if (const auto* update = std::get_if<UpdateOp>(&op)) {
				const UploadData data = update->dataCb();
				ASSERT_PASS(data.data, pass, "IntermediateBuilder::update called with null data.");
				ASSERT_PASS(data.size > 0 && data.size <= 65536, pass, "IntermediateBuilder::update requires 0 < size <= 65'536 (64KB). Please call IntermediateBuilder::upload instead!");
				ASSERT_PASS(data.size % 4 == 0, pass, "IntermediateBuilder::update size must be a multiple of 4 bytes.");
				ASSERT_PASS(update->dstOffset % 4 == 0, pass, "IntermediateBuilder::update offset must be a multiple of 4 bytes.");

				AllocatedBuffer& buf = cmd._ctx->access(update->dst);
				ASSERT_PASS(update->dstOffset + data.size <= buf._bufferSize, pass, "IntermediateBuilder::update: offset + size exceeds buffer '{}' size.", buf._debugName);
				ASSERT_PASS(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, pass, "IntermediateBuilder::update: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);

				vkCmdUpdateBuffer(cmd._wrapper->_cmdBuf, buf._vkBuffer, update->dstOffset, data.size, data.data);
			} else if (const auto* upload = std::get_if<UploadOp>(&op)) {
				const UploadData data = upload->dataCb();
				ASSERT_PASS(data.data, pass, "IntermediateBuilder::upload called with null data.");
				ASSERT_PASS(data.size > 0, pass, "IntermediateBuilder::upload size must be greater than 0.");

				AllocatedBuffer& buf = cmd._ctx->access(upload->dst);
				ASSERT_PASS(upload->dstOffset + data.size <= buf._bufferSize, pass, "IntermediateBuilder::upload: offset + size exceeds buffer '{}' size.", buf._debugName);
				ASSERT_PASS(
				        buf.isMapped() || (buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), pass, "IntermediateBuilder::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName
				);
				if (buf.isMapped()) {
					buf.bufferSubData(*cmd._ctx, upload->dstOffset, data.size, data.data);
					continue;
				}
				auto copies = cmd._ctx->_staging->stageBufferCopy(buf, data.data, data.size, upload->dstOffset);
				cmd._ctx->_staging->recordBufferCopies(cmd._wrapper->_cmdBuf, copies);
			}
		}
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
	TextureShape RenderGraph::shapeOf(const CTX& rCtx, const GraphTexture texture) const {
		if (const auto* declared = std::get_if<DeclaredTexture>(&ResourceOf(_textures, texture).source)) {
			const GraphTextureSpec& spec = declared->spec;
			return {
			    .debugName = spec.debugName,
			    .format = spec.format,
			    .sampleCount = static_cast<uint8_t>(toVulkan(spec.samples)),
			    .dimensions = spec.size,
			    .numMips = spec.numMipLevels,
			    .numLayers = spec.type == TextureType::Type_Cube ? 6 * spec.numLayers : spec.numLayers,
			};
		}
		const AllocatedTexture& live = rCtx.view(ResolveHandle(_textures, rCtx, texture));
		return {
		    .debugName = live.getDebugName(),
		    .format = live.getFormat(),
		    .sampleCount = live.getSampleCount(),
		    .dimensions = live.getDimensions(),
		    .numMips = live.getNumMips(),
		    .numLayers = live.getNumLayers(),
		};
	}

	static SubresourceRange ResolveSubresourceRange(const TextureDesc& texDesc, const TextureShape& shape, const PassDesc& pass) {
		const uint32_t baseMip = texDesc.baseLevel.value_or(0);
		const uint32_t numMips = texDesc.numLevels.value_or(texDesc.baseLevel.has_value() ? 1 : shape.numMips);
		const uint32_t baseLayer = texDesc.baseLayer.value_or(0);
		const uint32_t numLayers = texDesc.numLayers.value_or(texDesc.baseLayer.has_value() ? 1 : shape.numLayers);

		ASSERT_PASS(baseMip < shape.numMips, pass, "Texture '{}' base mip {} is outside the texture's {} mip levels", shape.debugName, baseMip, shape.numMips);
		ASSERT_PASS(
		        numMips > 0 && baseMip + numMips <= shape.numMips,
		        pass,
		        "Texture '{}' mip range [{}, {}) is outside the texture's {} mip levels",
		        shape.debugName,
		        baseMip,
		        baseMip + numMips,
		        shape.numMips
		);
		ASSERT_PASS(baseLayer < shape.numLayers, pass, "Texture '{}' base layer {} is outside the texture's {} layers", shape.debugName, baseLayer, shape.numLayers);
		ASSERT_PASS(
		        numLayers > 0 && baseLayer + numLayers <= shape.numLayers,
		        pass,
		        "Texture '{}' layer range [{}, {}) is outside the texture's {} layers",
		        shape.debugName,
		        baseLayer,
		        baseLayer + numLayers,
		        shape.numLayers
		);

		return {baseMip, numMips, baseLayer, numLayers};
	}

	static AttachmentTarget PlanAttachmentTarget(const TextureDesc& texDesc, const TextureShape& shape, const PassDesc& pass) {
		return {
		    .resource = texDesc.texture,
		    .range = ResolveSubresourceRange(texDesc, shape, pass),
		    .subresourceView = texDesc.baseLayer.has_value() || texDesc.baseLevel.has_value() || texDesc.numLayers.has_value() || texDesc.numLevels.has_value(),
		};
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
		const VkPipelineStageFlags2 shaderStages = vkutil::GetPassShaderStages(passType);
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
	void RenderGraph::planResourceAccess(const CTX& rCtx, const TextureDesc& texDesc, VkImageLayout desiredLayout, const StageAccess& dstMask, CompiledPass& outPass, const PassDesc& pass) const {
		const SubresourceRange range = ResolveSubresourceRange(texDesc, shapeOf(rCtx, texDesc.texture), pass);
		outPass.imageBarriers.push_back({.resource = texDesc.texture, .range = range, .dstLayout = desiredLayout, .dstMask = dstMask, .isSwapchain = IsBackBuffer(_textures, texDesc.texture)});
	}

	void RenderGraph::planPassResources(const CTX& rCtx, const PassDesc& passDesc, CompiledPass& outPass) const {
		outPass.imageBarriers.reserve(passDesc.textureDependencyOperations.size() + passDesc.attachmentOperations.size());
		outPass.bufferBarriers.reserve(passDesc.bufferDependencyOperations.size());
		for (const TextureDependencyDesc& dependency_desc: passDesc.textureDependencyOperations) {
			planResourceAccess(rCtx, dependency_desc.texDesc, GetTextureLayout(dependency_desc.access), GetTextureStageAccess(dependency_desc.access, passDesc.type), outPass, passDesc);
		}
		for (const AttachmentDesc& attachment_desc: passDesc.attachmentOperations) {
			const TextureShape shape = shapeOf(rCtx, attachment_desc.texDesc.texture);
			const VkImageLayout layout = vkutil::IsFormatDepth(shape.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			planResourceAccess(rCtx, attachment_desc.texDesc, layout, vkutil::GetPipelineStageAccess(layout), outPass, passDesc);

			if (attachment_desc.resolveTexDesc.has_value()) {
				// resolves always write in color attachment output, even for depth/stencil
				constexpr StageAccess resolveMask = {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
				planResourceAccess(rCtx, attachment_desc.resolveTexDesc.value(), layout, resolveMask, outPass, passDesc);
			}
		}
		for (const BufferDependencyDesc& dependency_desc: passDesc.bufferDependencyOperations) {
			outPass.bufferBarriers.push_back({.handle = dependency_desc.buffer.handle(), .dstMask = vkutil::GetBufferStageAccess(dependency_desc.access, passDesc.type)});
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

	void RenderGraph::planAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) const {
		if (pass_desc.attachmentOperations.empty())
			return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc: pass_desc.attachmentOperations) {
			const TextureDesc& texDesc = attachment_desc.texDesc;
			const TextureShape shape = shapeOf(rCtx, texDesc.texture);

			const bool isDepthAttachment = vkutil::IsFormatDepth(shape.format);
			if (isDepthAttachment) {
				ASSERT_PASS(!hasDepthAttachment, pass_desc, "Multiple depth attachments not allowed (found '{}' to be a second depth attachment)", shape.debugName);
				hasDepthAttachment = true;
			}

			const ClearValue& clear_value = attachment_desc.clearValue;
			AttachmentInfo attachment_info = {
			    .imageFormat = shape.format,
			    .imageView = VK_NULL_HANDLE,
			    .imageLayout = isDepthAttachment ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .resolveImageView = VK_NULL_HANDLE,
			    .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,

			    .loadOp = toVulkan(attachment_desc.loadOp),
			    .storeOp = toVulkan(attachment_desc.storeOp),
			    .clearValue = isDepthAttachment ? clear_value.getDepthStencilValue() : clear_value.getColorValue(),
			    .target = PlanAttachmentTarget(texDesc, shape, pass_desc),
			};
			if (attachment_desc.resolveTexDesc.has_value()) {
				const TextureDesc& resolve_desc = attachment_desc.resolveTexDesc.value();
				const TextureShape resolve_shape = shapeOf(rCtx, resolve_desc.texture);
				ASSERT_PASS(
				        resolve_shape.sampleCount == VK_SAMPLE_COUNT_1_BIT,
				        pass_desc,
				        "Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
				        resolve_shape.debugName
				);
				ASSERT_PASS(shape.sampleCount > VK_SAMPLE_COUNT_1_BIT, pass_desc, "Resolve operation on non-multisampled texture '{}'!", shape.debugName);
				ASSERT_PASS(
				        resolve_shape.format == shape.format,
				        pass_desc,
				        "Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
				        shape.debugName,
				        vkstring::VulkanFormatToString(shape.format),
				        resolve_shape.debugName,
				        vkstring::VulkanFormatToString(resolve_shape.format)
				);
				if (attachment_desc.storeOp == StoreOp::STORE)
					LOG_PASS(
					        LogType::Suggestion,
					        pass_desc,
					        "Attachment of texture '{}' has StoreOp::STORE and resolves into '{}', can be replaced with StoreOp::DONT_CARE.",
					        shape.debugName,
					        resolve_shape.debugName
					);

				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveTarget = PlanAttachmentTarget(resolve_desc, resolve_shape, pass_desc);
			}
			attachment_info.isSwapchainImage = IsBackBuffer(_textures, texDesc.texture);

			if (isDepthAttachment)
				outPass.depthAttachment.emplace(attachment_info);
			else
				outPass.colorAttachments.emplace_back(attachment_info);

			const uint32_t mipLevel = texDesc.baseLevel.value_or(0);
			const uint32_t width = std::max(1u, shape.dimensions.width >> mipLevel);
			const uint32_t height = std::max(1u, shape.dimensions.height >> mipLevel);

			if ((max_width != width || max_height != height) && (max_width != 0 || max_height != 0))
				LOG_PASS(LogType::Warning, pass_desc, "You have attachments of different dimensions, this is allowed but experimental.");
			max_width = std::max(max_width, width);
			max_height = std::max(max_height, height);
		}
		outPass.renderArea = {{0, 0}, {max_width, max_height}};
	}

	void RenderGraph::bindPass(CTX& rCtx, CompiledPass& pass) const {
		for (CompiledImageBarrier& barrier: pass.imageBarriers) {
			if (!barrier.isSwapchain)
				barrier.handle = ResolveHandle(_textures, rCtx, barrier.resource);
		}
		for (CompiledImageBarrier& barrier: pass.exitBarriers)
			barrier.handle = ResolveHandle(_textures, rCtx, barrier.resource);
		const auto bindView = [&](const TextureHandle handle, const AttachmentTarget& target) {
			const AllocatedTexture& texture = rCtx.view(handle);
			if (!target.subresourceView)
				return texture.getImageView();
			VkImageViewType viewType = texture.getViewType();
			if (target.range.numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE)
				viewType = VK_IMAGE_VIEW_TYPE_2D;
			const TextureViewKey viewKey = rCtx.createTextureView(handle, {
			    .type = viewType,
			    .mipLevel = target.range.baseMip,
			    .numMipLevels = target.range.numMips,
			    .layer = target.range.baseLayer,
			    .numLayers = target.range.numLayers,
			});
			return rCtx.getTextureView(handle, viewKey);
		};
		const auto bindAttachment = [&](AttachmentInfo& attachment) {
			attachment.texture = ResolveHandle(_textures, rCtx, attachment.target.resource);
			attachment.imageView = bindView(attachment.texture, attachment.target);
			if (attachment.resolveTarget.has_value())
				attachment.resolveImageView = bindView(ResolveHandle(_textures, rCtx, attachment.resolveTarget->resource), *attachment.resolveTarget);
			if (attachment.isSwapchainImage) {
				const auto& swapchain = rCtx._swapchain;
				const uint32_t numImages = swapchain->getNumOfSwapchainImages();
				for (uint32_t i = 0; i < numImages; i++)
					attachment.swapchainImageViews[i] = rCtx.view(swapchain->getSwapchainTextureHandle(i)).getImageView();
			}
		};
		for (AttachmentInfo& color: pass.colorAttachments)
			bindAttachment(color);
		if (pass.depthAttachment.has_value())
			bindAttachment(*pass.depthAttachment);
	}

	void RenderGraph::realizeDeclared(CTX& rCtx, const std::vector<uint32_t>& culledOrder) {
		std::vector<std::optional<TextureUses>> uses(_textures.size());
		const auto useOf = [&uses](const GraphTexture texture) -> TextureUses& {
			std::optional<TextureUses>& use = uses[IdOf(texture)];
			return use.has_value() ? *use : use.emplace();
		};
		for (const uint32_t pass_idx: culledOrder) {
			const PassDesc& desc = _passDescriptions[pass_idx];
			for (const AttachmentDesc& attachment: desc.attachmentOperations) {
				useOf(attachment.texDesc.texture).addAttachment();
				if (attachment.resolveTexDesc.has_value())
					useOf(attachment.resolveTexDesc->texture).addAttachment();
			}
			for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
				useOf(dependency.texDesc.texture).add(dependency.access);
		}
		for (uint32_t id = 0; id < _textures.size(); ++id) {
			auto* declared = std::get_if<DeclaredTexture>(&_textures[id].source);
			if (!declared || !uses[id].has_value())
				continue;
			const Realization wanted = InferRealization(declared->spec, *uses[id], declared->exported);
			ASSERT_MSG(wanted.usage != 0, "Graph texture '{}' is only touched by transfers, nothing samples, stores to or renders into it.", declared->spec.debugName);
			if (declared->texture.valid()) {
				ASSERT_MSG(declared->realized == wanted, "Graph texture '{}' needs different usage than it was created with, recreating graph textures is not supported yet.", declared->spec.debugName);
				continue;
			}
			declared->texture = rCtx.createTexture(ToTextureSpec(declared->spec, wanted));
			declared->realized = wanted;
		}
	}

	void RenderGraph::performDryRun(CTX& rCtx) {
		// preserve any in-flight command buffer (e.g. when compile() is triggered from inside
		// execute() after acquireCommand() has installed the real cmd buffer).
		const CommandBuffer saved = rCtx._currentCommandBuffer;
		for (const CompiledPass& pass: _compiledPasses) {
			if (pass.type == PassDesc::Type::Intermediate)
				continue;
			ASSERT_PASS(!pass.members.empty(), pass, "doesn't have an execute callback, something went horribly wrong!");
			for (const PassMember& member: pass.members) {
				// by default CommandBuffer will have _isDryRun = true
				CommandBuffer dryCmd;
				dryCmd._ctx = &rCtx;
				dryCmd._activePass = &pass;
				dryCmd._graph = this;
				dryCmd._activeMember = &member;
				dryCmd._viewMask = pass.viewMask;
				rCtx._currentCommandBuffer = dryCmd;
				member.callback(dryCmd);
			}
		}
		rCtx._currentCommandBuffer = saved;
	}

	bool isWrite(TextureAccess access) {
		return access == TextureAccess::StorageWrite || access == TextureAccess::StorageReadWrite || access == TextureAccess::TransferWrite;
	}
	bool isWrite(BufferAccess access) {
		return access == BufferAccess::ShaderReadWrite || access == BufferAccess::ShaderWrite || access == BufferAccess::TransferWrite;
	}

	static bool SameAttachmentImage(const AttachmentInfo& a, const AttachmentInfo& b) {
		return a.target.resource == b.target.resource && a.target.range == b.target.range;
	}
	// resolves run at EndRendering, so a later member may not keep drawing into an attachment the group already resolves
	static bool CanContinueAttachment(const AttachmentInfo& group, const AttachmentInfo& next) {
		return SameAttachmentImage(group, next) && next.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR && !group.resolveTarget.has_value();
	}

	struct PassResourceUse {
		std::unordered_set<GraphTexture, GraphTextureHash> attachments;
		std::unordered_map<GraphTexture, bool, GraphTextureHash> textures;
		std::unordered_map<BufferHandle, bool> buffers;

		void add(const PassDesc& desc) {
			for (const AttachmentDesc& attachment: desc.attachmentOperations) {
				attachments.insert(attachment.texDesc.texture);
				if (attachment.resolveTexDesc.has_value())
					attachments.insert(attachment.resolveTexDesc->texture);
			}
			for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
				textures[dependency.texDesc.texture] |= isWrite(dependency.access);
			for (const BufferDependencyDesc& dependency: desc.bufferDependencyOperations)
				buffers[dependency.buffer.handle()] |= isWrite(dependency.access);
		}
	};

	static std::vector<GraphTexture> SortedDeclaredTextures(const PassDesc& desc) {
		std::vector<GraphTexture> declared;
		for (const AttachmentDesc& attachment: desc.attachmentOperations) {
			declared.push_back(attachment.texDesc.texture);
			if (attachment.resolveTexDesc.has_value())
				declared.push_back(attachment.resolveTexDesc->texture);
		}
		for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
			declared.push_back(dependency.texDesc.texture);
		std::ranges::sort(declared, {}, IdOf);
		const auto [first, last] = std::ranges::unique(declared);
		declared.erase(first, last);
		return declared;
	}

	// any of these would need a barrier between members, which a single rendering scope cannot hold
	static bool HasMemberHazard(const PassResourceUse& group, const PassResourceUse& next) {
		const auto writeConflict = [](const auto& a, const auto& b) {
			for (const auto& [handle, written]: a) {
				if (const auto it = b.find(handle); it != b.end() && (written || it->second))
					return true;
			}
			return false;
		};
		for (const auto& [handle, written]: next.textures) {
			if (group.attachments.contains(handle))
				return true;
		}
		for (const auto& [handle, written]: group.textures) {
			if (next.attachments.contains(handle))
				return true;
		}
		return writeConflict(next.textures, group.textures) || writeConflict(next.buffers, group.buffers);
	}

	struct CompilingPass {
		CompiledPass pass;
		PassResourceUse use;
		std::vector<uint32_t> passIndices;
	};

	// leaves both records untouched when it returns false, the hoist lookahead relies on that
	static bool TryMergeInto(CompilingPass& groupRecord, CompilingPass& nextRecord) {
		CompiledPass& group = groupRecord.pass;
		CompiledPass& next = nextRecord.pass;
		const bool hasAttachments = !group.colorAttachments.empty() || group.depthAttachment.has_value();
		const bool compatible = hasAttachments && group.type == PassDesc::Type::Graphics && next.type == group.type && next.queue == group.queue &&
		                        !group.conditionCallback && !next.conditionCallback && !group.inOrderBarrier && !next.inOrderBarrier &&
		                        next.layerCount == group.layerCount && next.viewMask == group.viewMask &&
		                        next.renderArea.offset.x == group.renderArea.offset.x && next.renderArea.offset.y == group.renderArea.offset.y &&
		                        next.renderArea.extent.width == group.renderArea.extent.width && next.renderArea.extent.height == group.renderArea.extent.height &&
		                        next.colorAttachments.size() == group.colorAttachments.size() &&
		                        // pipelines that bring their own formats (ImGui) cannot draw with an attachment they never declared
		                        next.depthAttachment.has_value() == group.depthAttachment.has_value();
		if (!compatible)
			return false;
		for (size_t i = 0; i < group.colorAttachments.size(); ++i) {
			if (!CanContinueAttachment(group.colorAttachments[i], next.colorAttachments[i]))
				return false;
		}
		if (group.depthAttachment.has_value() && !CanContinueAttachment(*group.depthAttachment, *next.depthAttachment))
			return false;
		if (HasMemberHazard(groupRecord.use, nextRecord.use))
			return false;

		const auto findImageBarrier = [&](const CompiledImageBarrier& request) {
			return std::ranges::find_if(group.imageBarriers, [&](const CompiledImageBarrier& existing) { return existing.resource == request.resource; });
		};
		for (const CompiledImageBarrier& request: next.imageBarriers) {
			const auto it = findImageBarrier(request);
			if (it != group.imageBarriers.end() && (it->range != request.range || it->dstLayout != request.dstLayout))
				return false;
		}

		for (const CompiledImageBarrier& request: next.imageBarriers) {
			if (const auto it = findImageBarrier(request); it != group.imageBarriers.end()) {
				it->dstMask.stage |= request.dstMask.stage;
				it->dstMask.access |= request.dstMask.access;
			} else {
				group.imageBarriers.push_back(request);
			}
		}
		for (const CompiledBufferBarrier& request: next.bufferBarriers) {
			const auto it = std::ranges::find_if(group.bufferBarriers, [&](const CompiledBufferBarrier& existing) { return existing.handle == request.handle; });
			if (it != group.bufferBarriers.end()) {
				it->dstMask.stage |= request.dstMask.stage;
				it->dstMask.access |= request.dstMask.access;
			} else {
				group.bufferBarriers.push_back(request);
			}
		}
		const auto continueAttachment = [](AttachmentInfo& into, const AttachmentInfo& from) {
			into.storeOp = from.storeOp;
			into.resolveTarget = from.resolveTarget;
			into.resolveImageLayout = from.resolveImageLayout;
		};
		for (size_t i = 0; i < group.colorAttachments.size(); ++i)
			continueAttachment(group.colorAttachments[i], next.colorAttachments[i]);
		if (group.depthAttachment.has_value())
			continueAttachment(*group.depthAttachment, *next.depthAttachment);
		group.name += " + " + next.name;
		std::ranges::move(next.members, std::back_inserter(group.members));
		std::ranges::move(next.exitBarriers, std::back_inserter(group.exitBarriers));
		PassResourceUse& groupUse = groupRecord.use;
		groupUse.attachments.insert(nextRecord.use.attachments.begin(), nextRecord.use.attachments.end());
		for (const auto& [handle, written]: nextRecord.use.textures)
			groupUse.textures[handle] |= written;
		for (const auto& [handle, written]: nextRecord.use.buffers)
			groupUse.buffers[handle] |= written;
		groupRecord.passIndices.insert(groupRecord.passIndices.end(), nextRecord.passIndices.begin(), nextRecord.passIndices.end());
		return true;
	}

	static bool TryHoistIntoGroup(std::vector<CompilingPass>& compiled, std::vector<CompilingPass>& pending, size_t& pendingIdx, const std::vector<std::unordered_set<uint32_t>>& predecessors, bool logMerges) {
		CompilingPass& group = compiled.back();
		if (group.pass.type != PassDesc::Type::Graphics)
			return false;
		const auto dependsOnGroup = [&](const CompilingPass& candidate) {
			for (const uint32_t pass_idx: candidate.passIndices) {
				for (const uint32_t pred: predecessors[pass_idx]) {
					if (std::ranges::find(group.passIndices, pred) != group.passIndices.end())
						return true;
				}
			}
			return false;
		};
		for (size_t joinIdx = pendingIdx; joinIdx < pending.size(); ++joinIdx) {
			if (joinIdx > pendingIdx && TryMergeInto(group, pending[joinIdx])) {
				if (logMerges) {
					for (size_t hoistIdx = pendingIdx; hoistIdx < joinIdx; ++hoistIdx)
						LOG_PASS(LogType::Info, pending[hoistIdx].pass, "moved ahead of '{}' so it renders as one scope.", group.pass.name);
				}
				group.pass.hoistedAheadCount += static_cast<uint32_t>(joinIdx - pendingIdx);
				compiled.insert(compiled.end() - 1, std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(pendingIdx)), std::make_move_iterator(pending.begin() + static_cast<std::ptrdiff_t>(joinIdx)));
				pendingIdx = joinIdx;
				return true;
			}
			// only unconditional intermediate passes move, their upload data is checked in debug execute
			const CompiledPass& candidate = pending[joinIdx].pass;
			if (candidate.type != PassDesc::Type::Intermediate || candidate.conditionCallback || dependsOnGroup(pending[joinIdx]))
				return false;
		}
		return false;
	}

	struct DirectedGraph {
		// indicies represent original pass index
		// predecessors[2] = {0, 1} represents the third pass that must wait until first and second run
		// will always be same size vector as the # of passes
		std::vector<std::unordered_set<uint32_t>> predecessors;

		explicit DirectedGraph(uint32_t vertexCount) : predecessors(vertexCount) {}

		void addEdge(uint32_t from, uint32_t to) {
			if (from == to) return;
			ASSERT_MSG(from < to, "render graph edges must point from an earlier pass to a later one, compile relies on declaration order being topological");
			predecessors.at(to).insert(from);
		}
	};

	DirectedGraph build_directed_graph(const std::vector<PassDesc>& pass_descs, const size_t texture_count) {
		const size_t pass_desc_count = pass_descs.size();

		auto collectTexAccess = [](const PassDesc& desc) {
			std::vector<std::pair<uint32_t, bool>> result;
			result.reserve(desc.textureDependencyOperations.size() + desc.attachmentOperations.size());
			for (const auto& texDep : desc.textureDependencyOperations) {
				result.emplace_back(IdOf(texDep.texDesc.texture), isWrite(texDep.access));
			}
			for (const auto& attachDep : desc.attachmentOperations) {
				result.emplace_back(IdOf(attachDep.texDesc.texture), true);
				if (attachDep.resolveTexDesc.has_value())
					result.emplace_back(IdOf(attachDep.resolveTexDesc->texture), true);
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

		constexpr uint32_t kNoWriter = UINT32_MAX;
		std::vector<uint32_t> lastTexWriter(texture_count, kNoWriter);
		std::vector<std::vector<uint32_t>> lastTexReaders(texture_count);

		std::unordered_map<BufferHandle, uint32_t> lastBufWriter;
		std::unordered_map<BufferHandle, std::vector<uint32_t>> lastBufReaders;

		DirectedGraph result(pass_desc_count);
		// first collect all edges from the graph sequentially
		for (int i = 0; i < pass_desc_count; i++) {
			const PassDesc& desc = pass_descs[i];
			for (const auto& [h, isWrite] : collectTexAccess(desc)) {
				if (lastTexWriter[h] != kNoWriter)
					result.addEdge(lastTexWriter[h], i);

				if (isWrite) {
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

	static std::optional<uint32_t> LastWriter(const std::vector<PassDesc>& pass_descs, const uint32_t id) {
		std::optional<uint32_t> writer;
		for (uint32_t i = 0; i < pass_descs.size(); ++i) {
			const PassDesc& desc = pass_descs[i];
			for (const AttachmentDesc& attachment: desc.attachmentOperations) {
				if (IdOf(attachment.texDesc.texture) == id || (attachment.resolveTexDesc.has_value() && IdOf(attachment.resolveTexDesc->texture) == id))
					writer = i;
			}
			for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations) {
				if (IdOf(dependency.texDesc.texture) == id && isWrite(dependency.access))
					writer = i;
			}
		}
		return writer;
	}

	static std::unordered_set<uint32_t> BackBufferAndExportWriters(const std::vector<PassDesc>& pass_descs, const std::vector<GraphResource>& textures) {
		std::unordered_set<uint32_t> sinkPasses;
		for (uint32_t i = 0; i < pass_descs.size(); i++) {
			const PassDesc& pass_desc = pass_descs[i];
			for (const auto& attachDesc : pass_desc.attachmentOperations) {
				if (IsBackBuffer(textures, attachDesc.texDesc.texture)) {
					sinkPasses.insert(i);
					break;
				}
			}
			for (const auto& texDesc : pass_desc.textureDependencyOperations) {
				if (IsBackBuffer(textures, texDesc.texDesc.texture) && isWrite(texDesc.access)) {
					sinkPasses.insert(i);
					break;
				}
			}
		}
		for (uint32_t id = 0; id < textures.size(); ++id) {
			const auto* declared = std::get_if<DeclaredTexture>(&textures[id].source);
			if (!declared || !declared->exported)
				continue;
			const std::optional<uint32_t> writer = LastWriter(pass_descs, id);
			ASSERT_MSG(writer.has_value(), "Exported graph texture '{}' is never written by a pass.", declared->spec.debugName);
			sinkPasses.insert(*writer);
		}
		return sinkPasses;
	}

	std::vector<uint32_t> cull_unused_passes(
		const std::vector<uint32_t>& topo_sorted_order,
		const std::vector<std::unordered_set<uint32_t>>& predecessors,
		const std::vector<PassDesc>& pass_descs,
		const std::vector<GraphResource>& textures) {
		const std::unordered_set<uint32_t> sinkPasses = BackBufferAndExportWriters(pass_descs, textures);
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
		for (const GraphResource& resource: _textures) {
			if (const auto* imported = std::get_if<ImportedTexture>(&resource.source))
				ASSERT_MSG(rCtx._texturePool.get(imported->handle) != nullptr, "A texture imported into the RenderGraph was destroyed while the graph still uses it.");
		}

		const DirectedGraph directed_graph = build_directed_graph(this->_passDescriptions, _textures.size());
		const std::vector<std::unordered_set<uint32_t>>& passPredecessors = directed_graph.predecessors;
		std::vector<uint32_t> declaration_order(this->_passDescriptions.size());
		std::iota(declaration_order.begin(), declaration_order.end(), 0u);
		const std::vector<uint32_t> culled_order = cull_unused_passes(declaration_order, passPredecessors, _passDescriptions, _textures);

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

		std::vector<std::pair<uint32_t, GraphTexture>> exportWriters;
		for (uint32_t id = 0; id < _textures.size(); ++id) {
			if (const auto* declared = std::get_if<DeclaredTexture>(&_textures[id].source); declared && declared->exported)
				exportWriters.emplace_back(*LastWriter(_passDescriptions, id), GraphTexture{id});
		}

		// compile works in this order
		std::vector<CompilingPass> pending;
		pending.reserve(culled_order.size());
		for (const uint32_t pass_idx : culled_order) {
			const PassDesc& pass_desc = this->_passDescriptions[pass_idx];
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.queue = sanitizeQueue(pass_desc.queue, pass_desc);
			compiled_pass.type = pass_desc.type;
			if (pass_desc.executeCallback)
				compiled_pass.members.push_back({pass_desc.name, pass_desc.executeCallback, SortedDeclaredTextures(pass_desc)});
			compiled_pass.intermediateOps = pass_desc.intermediateOps;
			compiled_pass.layerCount = pass_desc.layerCount;
			compiled_pass.viewMask = pass_desc.viewMask;
			compiled_pass.conditionCallback = pass_desc.conditionCallback;

			planPassResources(rCtx, pass_desc, compiled_pass);
			planAttachments(rCtx, pass_desc, compiled_pass);
			if (pass_desc.executeInOrder)
				compiled_pass.inOrderBarrier = BuildInOrderBarrier(compiled_pass);
			for (const auto& [writer, texture]: exportWriters) {
				if (writer != pass_idx)
					continue;
				const TextureShape shape = shapeOf(rCtx, texture);
				compiled_pass.exitBarriers.push_back({
				    .resource = texture,
				    .range = {0, shape.numMips, 0, shape.numLayers},
				    .dstLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .dstMask = {.stage = vkutil::GetPassShaderStages(PassDesc::Type::Graphics) | vkutil::GetPassShaderStages(PassDesc::Type::Compute), .access = VK_ACCESS_2_SHADER_READ_BIT},
				});
			}

			CompilingPass& record = pending.emplace_back(std::move(compiled_pass));
			record.use.add(pass_desc);
			record.passIndices.push_back(pass_idx);
		}

		// recompiles on every resize, so only the first compile reports what it merged
		const bool logMerges = !this->_hasCompiled;
		std::vector<CompilingPass> compiled;
		compiled.reserve(pending.size());
		for (size_t pendingIdx = 0; pendingIdx < pending.size(); ++pendingIdx) {
			if (!compiled.empty() && (TryMergeInto(compiled.back(), pending[pendingIdx]) || TryHoistIntoGroup(compiled, pending, pendingIdx, passPredecessors, logMerges)))
				continue;
			compiled.push_back(std::move(pending[pendingIdx]));
		}
		realizeDeclared(rCtx, culled_order);
		for (CompilingPass& record: compiled)
			bindPass(rCtx, record.pass);
#ifdef DEBUG
		{
			// memoryless attachments have no backing memory, so their contents must begin and end inside one rendering scope
			std::unordered_map<GraphTexture, uint32_t, GraphTextureHash> passesUsing;
			for (const CompilingPass& record: compiled) {
				for (const GraphTexture texture: record.use.attachments)
					++passesUsing[texture];
				for (const auto& [texture, written]: record.use.textures)
					++passesUsing[texture];
			}
			for (const CompilingPass& record: compiled) {
				const auto validate = [&](const AttachmentInfo& attachment) {
					if (attachment.isSwapchainImage)
						return;
					const AllocatedTexture& texture = rCtx.view(attachment.texture);
					const bool confined = passesUsing[attachment.target.resource] == 1 && attachment.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && attachment.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
					if (texture.isMemoryless()) {
						ASSERT_PASS(confined, record.pass, "Memoryless attachment '{}' must be used by a single rendering scope that clears or discards it on load and discards it on store.", texture.getDebugName());
					} else if (confined && logMerges && !texture.isSampledImage() && !texture.isStorageImage()) {
						LOG_PASS(LogType::Suggestion, record.pass, "Attachment '{}' never leaves this rendering scope and can be StorageType::Memoryless.", texture.getDebugName());
					}
				};
				for (const AttachmentInfo& color: record.pass.colorAttachments)
					validate(color);
				if (record.pass.depthAttachment.has_value())
					validate(*record.pass.depthAttachment);
			}
		}
#endif

		constexpr uint32_t kUnscheduled = UINT32_MAX;
		std::vector<uint32_t> descToCompiled(this->_passDescriptions.size(), kUnscheduled);
		this->_compiledPasses.reserve(compiled.size());
		for (CompilingPass& record: compiled) {
			for (const uint32_t pass_idx: record.passIndices)
				descToCompiled[pass_idx] = static_cast<uint32_t>(this->_compiledPasses.size());
			if (logMerges && record.pass.members.size() > 1)
				LOG_PASS(LogType::Info, record.pass, "merged {} passes into one rendering scope.", record.pass.members.size());
			this->_compiledPasses.push_back(std::move(record.pass));
		}

		// schedule over compiled passes so a merged group occupies a single node
		std::vector<std::unordered_set<uint32_t>> compiledPredecessors(this->_compiledPasses.size());
		for (const uint32_t pass_idx : culled_order) {
			const uint32_t compiled_idx = descToCompiled[pass_idx];
			for (const uint32_t pred : passPredecessors[pass_idx]) {
				const uint32_t compiled_pred = descToCompiled[pred];
				if (compiled_pred != kUnscheduled && compiled_pred != compiled_idx)
					compiledPredecessors[compiled_idx].insert(compiled_pred);
			}
		}
		std::vector<uint32_t> compiled_order(this->_compiledPasses.size());
		std::iota(compiled_order.begin(), compiled_order.end(), 0u);
		this->_schedule = calculate_execution_schedule(compiled_order, compiledPredecessors);

		// requires that correct queues have already been selected
		this->_layerByQueue.assign(this->_schedule.layers.size(), {});
		for (size_t depth = 0; depth < this->_schedule.layers.size(); ++depth) {
			for (const uint32_t compiled_idx : this->_schedule.layers[depth]) {
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

	void RenderGraph::PerformBarrierTransitions(CommandBuffer& cmd, const std::span<const CompiledImageBarrier> imageBarriers, const std::span<const CompiledBufferBarrier> bufferBarriers) {
		if (imageBarriers.empty() && bufferBarriers.empty())
			return;
		ASSERT(cmd._wrapper->_cmdBuf);

		// per-pass scratch lives on the stack and only spills to the heap for unusually large passes
		std::array<std::byte, 8192> scratch;
		std::pmr::monotonic_buffer_resource arena{scratch.data(), scratch.size()};
		std::pmr::vector<VkImageMemoryBarrier2> vkImageBarriers{&arena};
		vkImageBarriers.reserve(imageBarriers.size());
		std::pmr::vector<VkBufferMemoryBarrier2> vkBufferBarriers{&arena};
		vkBufferBarriers.reserve(bufferBarriers.size());
		std::pmr::vector<SubresourceMap<StageAccess>::Entry> mergedReads{&arena};
		for (const CompiledImageBarrier& req: imageBarriers) {
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
		for (const CompiledBufferBarrier& req: bufferBarriers) {
			const std::optional<vkutil::BufferBarrier> barrier = vkutil::TrackBufferAccess(_bufferTrackers[req.handle], req.dstMask);
			if (!barrier)
				continue;
			const AllocatedBuffer& buffer = cmd._ctx->view(req.handle);
			vkBufferBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = barrier->src.stage,
				.srcAccessMask = barrier->src.access,
				.dstStageMask = barrier->dst.stage,
				.dstAccessMask = barrier->dst.access,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buffer._vkBuffer,
				.offset = 0,
				.size = VK_WHOLE_SIZE}
			);
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

#ifdef DEBUG
	static uint64_t FingerprintUploadData(const CompiledPass& pass) {
		uint64_t hash = 14695981039346656037ull;
		const auto mix = [&hash](const void* bytes, size_t size) {
			for (size_t i = 0; i < size; ++i)
				hash = (hash ^ static_cast<const uint8_t*>(bytes)[i]) * 1099511628211ull;
		};
		for (const IntermediateOp& op: pass.intermediateOps) {
			UploadData data{};
			if (const auto* update = std::get_if<UpdateOp>(&op))
				data = update->dataCb();
			else if (const auto* upload = std::get_if<UploadOp>(&op))
				data = upload->dataCb();
			else
				continue;
			mix(&data.size, sizeof(data.size));
			if (data.data)
				mix(data.data, data.size);
		}
		return hash;
	}
#endif

	void RenderGraph::execute(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		// auto-recompile if any resource topology changed since last compile (texture resize, swapchain recreate/destroy).
		// single uint64 compare on the hot path; recompile only runs when actually stale (typically window-resize frames).
		if (_compiledEpoch != cmd._ctx->_resourceEpoch) {
			compile(*cmd._ctx);
		}

		for (size_t passIdx = 0; passIdx < _compiledPasses.size(); ++passIdx) {
			CompiledPass& pass = _compiledPasses[passIdx];
			if (pass.conditionCallback && !pass.conditionCallback())
				continue;
			const bool isGraphics = pass.type == PassDesc::Type::Graphics;
#ifdef DEBUG
			// hoisted uploads ran before this group's callbacks, they must not read CPU state those callbacks write
			std::vector<uint64_t> hoistedFingerprints;
			for (uint32_t i = 0; i < pass.hoistedAheadCount; ++i)
				hoistedFingerprints.push_back(FingerprintUploadData(_compiledPasses[passIdx - pass.hoistedAheadCount + i]));
#endif
			// perform batched vkCmdPipelineBarrier
			PerformBarrierTransitions(cmd, pass.imageBarriers, pass.bufferBarriers);
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
			cmd._activePass = &pass;
			cmd._graph = this;
			// clear the previous pass's pipeline before begin rendering inspects it
			cmd.resetMemberStateImpl();
			if (isGraphics) cmd.cmdBeginRenderingEXT(pass.layerCount, pass.viewMask);
			if (pass.type == PassDesc::Type::Intermediate)
				RecordIntermediateOps(cmd, pass);
			for (size_t i = 0; i < pass.members.size(); ++i) {
				if (i > 0)
					cmd.resetMemberStateImpl();
				cmd._activeMember = &pass.members[i];
				pass.members[i].callback(cmd);
			}
			if (isGraphics) cmd.cmdEndRenderingEXT();
			PerformBarrierTransitions(cmd, pass.exitBarriers, {});
#ifdef DEBUG
			for (uint32_t i = 0; i < pass.hoistedAheadCount; ++i) {
				const CompiledPass& hoisted = _compiledPasses[passIdx - pass.hoistedAheadCount + i];
				ASSERT_PASS(
				        FingerprintUploadData(hoisted) == hoistedFingerprints[i],
				        hoisted,
				        "was moved ahead of '{}' to share its rendering scope, but its upload data changed while that scope recorded. Declare it before '{}' or prepare its data before RenderGraph::execute.",
				        pass.name,
				        pass.name
				);
			}
#endif
		}
		if (!cmd._ctx->isHeadless()) {
			// fixme: make this cleaner im so lazy right now
			cmd.cmdTransitionLayout(cmd._ctx->getCurrentSwapchainTexHandle(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		}
		cmd._graph = nullptr;
		cmd._activeMember = nullptr;
	}
} // namespace mythril
