//
// Created by Hayden Rivas on 10/11/25.
//

#include "HandleAccess.h"
#include "mythril/CommandBuffer.h"
#include "mythril/vkenums.h"

#include "mythril/RenderGraphBuilder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory_resource>
#include <numeric>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <queue>
#include <ranges>

#include "RenderGraphInternal.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include <vulkan/vk_enum_string_helper.h>

#include "GPUProfiler.h"
#include "ImmediateCommands.h"
#include "StagingDevice.h"
#include "Swapchain.h"
#include "mythril/CPUProfiler.h"

namespace mythril {
	using namespace detail;
	RenderGraph::RenderGraph() = default;
	RenderGraph::~RenderGraph() = default;
	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	static GraphTexture BaseOf(const GraphTexture texture) {
		return GraphTextureAccess::make(IdOf(texture));
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
			return declared->texture;
		}
		return rCtx.getCurrentSwapchainTexHandle();
	}

	ResolvedTexture RenderGraph::resolve(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, const GraphTexture texture) const {
		const TextureHandle handle = ResolveHandle(_textures, rCtx, texture);
#ifdef DEBUG
		ASSERT_PASS(std::ranges::binary_search(member.sortedDeclaredTextures, IdOf(texture), {}, IdOf), pass, "'{}' reads texture '{}' without declaring it.", member.name, rCtx.view(handle).getDebugName());
#endif
		return { handle, rCtx.getTextureDimensions(handle) };
	}

	GraphTexture RenderGraph::importTexture(const Texture& texture) {
		ASSERT_MSG(texture.valid(), "RenderGraph::importTexture called with an invalid texture.");
		ASSERT_MSG(!texture.getContext()->isSwapchainTexture(texture.handle()), "Swapchain images are not imported, use RenderGraph::backBuffer().");
		for (uint32_t i = 0; i < _textures.size(); ++i) {
			if (const auto* imported = std::get_if<ImportedTexture>(&_textures[i].source); imported && imported->handle == texture.handle())
				return GraphTexture{ i };
		}
		const AllocatedTexture& live = texture.getContext()->view(texture.handle());
		_textures.push_back({ ImportedTexture{ texture.handle(), std::string(live.getDebugName()), live.getNumMips() } });
		return GraphTexture{ static_cast<uint32_t>(_textures.size() - 1) };
	}

	GraphTexture RenderGraph::importTexture(const Texture& texture, const ExternalAccess& external) {
		const GraphTexture imported = importTexture(texture);
		_textures[IdOf(imported)].external = external;
		return imported;
	}
	GraphBuffer RenderGraph::importBuffer(const Buffer& buffer, const ExternalAccess& external) {
		const GraphBuffer imported = importBuffer(buffer);
		_buffers[GraphBufferAccess::index(imported)].external = external;
		return imported;
	}

	GraphBuffer RenderGraph::importBuffer(const Buffer& buffer) {
		ASSERT_MSG(buffer.valid(), "RenderGraph::importBuffer called with an invalid buffer.");
		for (uint32_t i = 0; i < _buffers.size(); ++i) {
			if (_buffers[i].handle == buffer.handle())
				return GraphBuffer{ i };
		}
		_buffers.push_back({ buffer.handle(), std::string(buffer.getContext()->view(buffer.handle()).getDebugName()) });
		return GraphBuffer{ static_cast<uint32_t>(_buffers.size() - 1) };
	}

	const std::vector<uint32_t>& RenderGraph::producersOf(const uint32_t id) const {
		static const std::vector<uint32_t> kNoProducers;
		return id < _bufferProducers.size() ? _bufferProducers[id] : kNoProducers;
	}
	std::optional<uint32_t> RenderGraph::producerOf(const GraphBuffer buffer) const {
		const std::vector<uint32_t>& producers = producersOf(GraphBufferAccess::index(buffer));
		const uint32_t version = GraphBufferAccess::version(buffer);
		if (version == 0 || version > producers.size())
			return std::nullopt;
		return producers[version - 1];
	}

	BufferHandle RenderGraph::handleOf(const GraphBuffer buffer) const {
		ASSERT_MSG(buffer.valid() && GraphBufferAccess::index(buffer) < _buffers.size(), "GraphBuffer does not belong to this RenderGraph.");
		return _buffers[GraphBufferAccess::index(buffer)].handle;
	}

	std::string_view RenderGraph::nameOf(const GraphTexture texture) const {
		const GraphResource& resource = ResourceOf(_textures, texture);
		if (const auto* declared = std::get_if<DeclaredTexture>(&resource.source))
			return declared->spec.debugName;
		if (const auto* imported = std::get_if<ImportedTexture>(&resource.source))
			return imported->debugName;
		return "back buffer";
	}

	uint32_t RenderGraph::mipCountOf(const GraphTexture texture) const {
		const GraphResource& resource = ResourceOf(_textures, texture);
		if (const auto* declared = std::get_if<DeclaredTexture>(&resource.source))
			return declared->spec.numMipLevels;
		if (const auto* imported = std::get_if<ImportedTexture>(&resource.source))
			return imported->numMips;
		return 1;
	}

	// the pass being declared is pushed after its setup, so it takes the next index
	static uint32_t MintVersion(std::vector<uint32_t>& producers, const uint32_t passIdx, const uint32_t version, const PassDesc& pass, const std::vector<PassDesc>& passes, const std::string_view name) {
		const auto latest = static_cast<uint32_t>(producers.size());
		if (!producers.empty() && producers.back() == passIdx && (version == latest || version + 1 == latest))
			return latest;
		ASSERT_PASS(version <= latest, pass, "writes version {} of '{}', which no pass produced this frame.", version, name);
		ASSERT_PASS(version == latest, pass, "writes on top of version {} of '{}', but '{}' already did. Write on top of the version it produced, or only read the old one.", version, name, passes[producers[version]].name);
		producers.push_back(passIdx);
		return latest + 1;
	}
	static void CheckReadVersion(const std::vector<uint32_t>& producers, const uint32_t version, const PassDesc& pass, const std::string_view name) {
		ASSERT_PASS(version <= producers.size(), pass, "reads version {} of '{}', which no pass produced this frame.", version, name);
	}
	GraphTexture RenderGraph::readVersion(const PassDesc& pass, const GraphTexture texture) const {
		ResourceOf(_textures, texture);
		static const std::vector<uint32_t> kNoProducers;
		CheckReadVersion(IdOf(texture) < _textureProducers.size() ? _textureProducers[IdOf(texture)] : kNoProducers, VersionOf(texture), pass, nameOf(texture));
		return texture;
	}
	GraphTexture RenderGraph::writeVersion(const PassDesc& pass, const GraphTexture texture) {
		ResourceOf(_textures, texture);
		if (_textureProducers.size() < _textures.size())
			_textureProducers.resize(_textures.size());
		const uint32_t version = MintVersion(_textureProducers[IdOf(texture)], static_cast<uint32_t>(_passDescriptions.size()), VersionOf(texture), pass, _passDescriptions, nameOf(texture));
		return GraphTextureAccess::make(IdOf(texture), version);
	}
	GraphBuffer RenderGraph::readVersion(const PassDesc& pass, const GraphBuffer buffer) const {
		handleOf(buffer);
		const uint32_t id = GraphBufferAccess::index(buffer);
		CheckReadVersion(producersOf(id), GraphBufferAccess::version(buffer), pass, _buffers[id].debugName);
		return buffer;
	}
	GraphBuffer RenderGraph::writeVersion(const PassDesc& pass, const GraphBuffer buffer) {
		handleOf(buffer);
		const uint32_t id = GraphBufferAccess::index(buffer);
		if (_bufferProducers.size() < _buffers.size())
			_bufferProducers.resize(_buffers.size());
		const uint32_t version = MintVersion(_bufferProducers[id], static_cast<uint32_t>(_passDescriptions.size()), GraphBufferAccess::version(buffer), pass, _passDescriptions, _buffers[id].debugName);
		return GraphBufferAccess::make(id, version);
	}

	GraphTexture BasePassBuilder::dependency(const TextureDesc& texDesc, const TextureAccess access) {
		_passSource.textureDependencyOperations.emplace_back(texDesc, access);
		return isWrite(access) ? _rGraph.writeVersion(_passSource, texDesc.texture) : _rGraph.readVersion(_passSource, texDesc.texture);
	}
	GraphBuffer BasePassBuilder::declare(const GraphBuffer buffer, const BufferAccess access) {
		_passSource.bufferDependencyOperations.push_back({ buffer, _rGraph.handleOf(buffer), access });
		return isWrite(access) ? _rGraph.writeVersion(_passSource, buffer) : _rGraph.readVersion(_passSource, buffer);
	}
	GraphBuffer BasePassBuilder::dependency(const GraphBuffer buffer, const BufferAccess access) {
		ASSERT_PASS(!IsStructureAccess(access), _passSource, "declares buffer '{}' with an acceleration structure access. Import the structure with importAccelerationStructure and declare that.", _rGraph._buffers[GraphBufferAccess::index(buffer)].debugName);
		return declare(buffer, access);
	}
	void BasePassBuilder::readDependencies(const std::span<const GraphTexture> textures, const TextureAccess access) {
		ASSERT_PASS(!isWrite(access), _passSource, "writes through a const texture array, which cannot take the new versions. Pass a mutable array.");
		for (const GraphTexture texture: textures)
			dependency(texture, access);
	}
	AttachmentOutput BasePassBuilder::attachment(const AttachmentDesc& desc) {
		_passSource.attachmentOperations.push_back(desc);
		AttachmentOutput out{ .texture = _rGraph.writeVersion(_passSource, desc.texDesc.texture) };
		if (desc.resolveTexDesc.has_value())
			out.resolve = _rGraph.writeVersion(_passSource, desc.resolveTexDesc->texture);
		return out;
	}

	GraphTexture RenderGraph::createTexture(GraphTextureSpec spec) {
		ASSERT_MSG(spec.format != VK_FORMAT_UNDEFINED, "RenderGraph::createTexture: '{}' needs a format.", spec.debugName);
		if (const auto* fixed = std::get_if<Dimensions>(&spec.size))
			ASSERT_MSG(fixed->width > 0 && fixed->height > 0 && fixed->depth > 0, "RenderGraph::createTexture: '{}' has a zero dimension.", spec.debugName);
		if (const auto* relative = std::get_if<RelativeSize>(&spec.size)) {
			ASSERT_MSG(relative->scale > 0.f, "RenderGraph::createTexture: '{}' needs a positive scale.", spec.debugName);
			ASSERT_MSG(relative->extent._index < _extents.size(), "RenderGraph::createTexture: '{}' follows an extent from another RenderGraph.", spec.debugName);
		}
		ASSERT_MSG(spec.numMipLevels > 0 && spec.numLayers > 0, "RenderGraph::createTexture: '{}' needs at least one mip level and layer.", spec.debugName);
		_textures.push_back({ DeclaredTexture{ .spec = std::move(spec) } });
		_hasCompiled = false;
		return GraphTexture{ static_cast<uint32_t>(_textures.size() - 1) };
	}

	GraphTexture RenderGraph::backBuffer() {
		for (uint32_t i = 0; i < _textures.size(); ++i) {
			if (std::holds_alternative<SwapchainTexture>(_textures[i].source))
				return GraphTexture{ i };
		}
		_textures.push_back({ SwapchainTexture{} });
		return GraphTexture{ static_cast<uint32_t>(_textures.size() - 1) };
	}

	GraphExtent RenderGraph::createExtent(const Dimensions dimensions) {
		ASSERT_MSG(dimensions.width > 0 && dimensions.height > 0, "RenderGraph::createExtent needs a non zero extent.");
		_extents.emplace_back(Dimensions{ dimensions.width, dimensions.height, 1 });
		return GraphExtent{ static_cast<uint32_t>(_extents.size() - 1) };
	}

	void RenderGraph::setExtent(const GraphExtent extent, const Dimensions dimensions) {
		ASSERT_MSG(extent != GraphExtent::swapchain(), "The swapchain extent follows CTX::recreateSwapchain and cannot be set.");
		ASSERT_MSG(extent._index < _extents.size(), "GraphExtent does not belong to this RenderGraph.");
		ASSERT_MSG(dimensions.width > 0 && dimensions.height > 0, "RenderGraph::setExtent needs a non zero extent.");
		_extents[extent._index] = Dimensions{ dimensions.width, dimensions.height, 1 };
	}

	Dimensions RenderGraph::extentOf(const CTX& rCtx, const GraphExtent extent) const {
		return _extents[extent._index].value_or(rCtx.getSwapchainDimensions());
	}

	Dimensions RenderGraph::sizeOf(const CTX& rCtx, const GraphTextureSpec& spec) const {
		if (const auto* relative = std::get_if<RelativeSize>(&spec.size))
			return ScaleExtent(extentOf(rCtx, relative->extent), relative->scale);
		return std::get<Dimensions>(spec.size);
	}

	BasePassBuilder::BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type) :
		_rGraph(rGraph),
		_passSource(pName, type) {
		ASSERT_MSG(!_passSource.name.empty(), "A pass needs a non empty name.");
	}

	GraphTexture TransferBuilder::blit(TextureDesc src, TextureDesc dst) {
		base.dependency(src, TextureAccess::TransferRead);
		const GraphTexture out = base.dependency(dst, TextureAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(BlitOp{ src.texture, dst.texture });
		return out;
	}
	GraphTexture TransferBuilder::copy(TextureDesc src, TextureDesc dst) {
		base.dependency(src, TextureAccess::TransferRead);
		const GraphTexture out = base.dependency(dst, TextureAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(CopyOp{ src.texture, dst.texture });
		return out;
	}

	GraphBuffer TransferBuilder::copy(const GraphBuffer src, const GraphBuffer dst) {
		ASSERT_PASS(base._rGraph.handleOf(src) != base._rGraph.handleOf(dst), base._passSource, "TransferBuilder::copy: source and destination are the same buffer.");
		base.dependency(src, BufferAccess::TransferRead);
		const GraphBuffer out = base.dependency(dst, BufferAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(BufferCopyOp{ base._rGraph.handleOf(src), base._rGraph.handleOf(dst) });
		return out;
	}
	GraphBuffer TransferBuilder::readback(const TextureDesc src, const GraphBuffer dst) {
		ASSERT_PASS(src.numLevels.value_or(1) == 1, base._passSource, "TransferBuilder::readback copies one mip level, pick it with baseLevel.");
		TextureDesc oneMip = src;
		oneMip.baseLevel = src.baseLevel.value_or(0);
		base.dependency(oneMip, TextureAccess::TransferRead);
		const GraphBuffer out = base.dependency(dst, BufferAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(ReadbackOp{ oneMip, base._rGraph.handleOf(dst) });
		return out;
	}
	GraphTexture TransferBuilder::upload(const TextureDesc dst, const std::span<const std::byte> bytes) {
		ASSERT_PASS(!bytes.empty(), base._passSource, "TransferBuilder::upload size must be greater than 0.");
		ASSERT_PASS(dst.numLevels.value_or(1) == 1, base._passSource, "TransferBuilder::upload writes one mip level, pick it with baseLevel.");
		TextureDesc oneMip = dst;
		oneMip.baseLevel = dst.baseLevel.value_or(0);
		const GraphTexture out = base.dependency(oneMip, TextureAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(ImageUploadOp{ oneMip, base._rGraph._arena.copy(bytes) });
		return out;
	}
	GraphTexture TransferBuilder::generateMipmaps(const GraphTexture texture) {
		const uint32_t numMips = base._rGraph.mipCountOf(texture);
		ASSERT_PASS(numMips > 1, base._passSource, "TransferBuilder::generateMipmaps: '{}' has only one mip level.", base._rGraph.nameOf(texture));
		TextureDesc top{ texture };
		top.baseLevel = 0;
		TextureDesc rest{ texture };
		rest.baseLevel = 1;
		rest.numLevels = numMips - 1;
		base.dependency(top, TextureAccess::TransferRead);
		const GraphTexture out = base.dependency(rest, TextureAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(GenerateMipsOp{ texture });
		return out;
	}
	GraphTexture TransferBuilder::dependency(const TextureDesc& texDesc, const TextureAccess access) {
		ASSERT_PASS(access == TextureAccess::TransferRead || access == TextureAccess::TransferWrite, base._passSource, "a transfer pass only declares transfer accesses.");
		return base.dependency(texDesc, access);
	}
	GraphBuffer TransferBuilder::dependency(const GraphBuffer buffer, const BufferAccess access) {
		ASSERT_PASS(access == BufferAccess::TransferRead || access == BufferAccess::TransferWrite, base._passSource, "a transfer pass only declares transfer accesses.");
		return base.dependency(buffer, access);
	}

	GraphBuffer TransferBuilder::update(const GraphBuffer buffer, const std::span<const std::byte> bytes, const size_t dstOffset) {
		ASSERT_PASS(!bytes.empty() && bytes.size() <= 65536, base._passSource, "TransferBuilder::update requires 0 < size <= 65'536 (64KB). Please call TransferBuilder::upload instead!");
		ASSERT_PASS(bytes.size() % 4 == 0, base._passSource, "TransferBuilder::update size must be a multiple of 4 bytes.");
		ASSERT_PASS(dstOffset % 4 == 0, base._passSource, "TransferBuilder::update offset must be a multiple of 4 bytes.");
		const GraphBuffer out = base.dependency(buffer, BufferAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(UpdateOp{ base._rGraph.handleOf(buffer), dstOffset, base._rGraph._arena.copy(bytes) });
		return out;
	}
	GraphBuffer TransferBuilder::upload(const GraphBuffer buffer, const std::span<const std::byte> bytes, const size_t dstOffset) {
		ASSERT_PASS(!bytes.empty(), base._passSource, "TransferBuilder::upload size must be greater than 0.");
		const GraphBuffer out = base.dependency(buffer, BufferAccess::TransferWrite);
		base._passSource.transferOps.emplace_back(UploadOp{ base._rGraph.handleOf(buffer), dstOffset, base._rGraph._arena.copy(bytes) });
		return out;
	}

	void RenderGraph::addPassDesc(PassDesc&& desc) {
		_passDescriptions.push_back(std::move(desc));
		_hasCompiled = false;
	}
	void RenderGraph::addTransferDesc(PassDesc&& desc) {
		ASSERT_PASS(!desc.transferOps.empty(), desc, "transfer pass has no operations!");
#ifdef DEBUG
		CheckBuildHazards(desc, _buffers);
#endif
		addPassDesc(std::move(desc));
	}

	static constexpr size_t kArenaBlockSize = 64 * 1024;
	void* PassArena::allocate(const size_t size, const size_t align) {
		ASSERT_MSG(align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "pass data alignment {} is above the arena's {}.", align, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		for (; _block < _blocks.size(); ++_block, _offset = 0) {
			const size_t start = (_offset + align - 1) & ~(align - 1);
			if (start + size <= _blocks[_block].size) {
				_offset = start + size;
				return _blocks[_block].bytes.get() + start;
			}
		}
		const size_t blockSize = std::max(size, kArenaBlockSize);
		_blocks.push_back({ std::make_unique<std::byte[]>(blockSize), blockSize });
		_offset = size;
		return _blocks.back().bytes.get();
	}
	std::span<const std::byte> PassArena::copy(const std::span<const std::byte> bytes) {
		auto* dst = static_cast<std::byte*>(allocate(bytes.size(), 1));
		std::memcpy(dst, bytes.data(), bytes.size());
		return { dst, bytes.size() };
	}
	void PassArena::clear() {
		for (auto& _destructor: std::views::reverse(_destructors))
			_destructor.destroy(_destructor.object);
		_destructors.clear();
		_block = 0;
		_offset = 0;
	}

	struct PackedMip {
		VkBufferImageCopy region;
		VkDeviceSize bytes;
	};
	static PackedMip PackMip(const AllocatedTexture& tex, const TextureDesc& desc, const CompiledPass& pass) {
		const uint32_t mip = desc.baseLevel.value_or(0);
		const uint32_t baseLayer = desc.baseLayer.value_or(0);
		const uint32_t numLayers = desc.numLayers.value_or(desc.baseLayer.has_value() ? 1 : tex.getNumLayers());
		const Dimensions full = tex.getDimensions();
		const VkExtent3D extent = { std::max(1u, full.width >> mip), std::max(1u, full.height >> mip), std::max(1u, full.depth >> mip) };
		const VkImageAspectFlags aspect = vkutil::AspectMaskFromFormat(tex.getFormat());
		ASSERT_PASS(std::has_single_bit(aspect), pass, "'{}' has depth and stencil, which one copy cannot cover.", tex.getDebugName());
		return {
			.region = { .imageSubresource = { aspect, mip, baseLayer, numLayers }, .imageExtent = extent },
			.bytes = VkDeviceSize{ extent.width } * extent.height * extent.depth * numLayers * vkutil::GetBytesPerPixel(tex.getFormat()),
		};
	}
	void RenderGraph::RecordReadback(CommandBuffer& cmd, const CompiledPass& pass, const ReadbackOp& op) const {
		const AllocatedTexture& tex = cmd._ctx->view(ResolveHandle(_textures, *cmd._ctx, op.src.texture));
		const AllocatedBuffer& buf = cmd._ctx->view(op.dst);
		const PackedMip packed = PackMip(tex, op.src, pass);
		ASSERT_PASS(packed.bytes <= buf._bufferSize, pass, "TransferBuilder::readback: '{}' needs {} bytes, '{}' has {}.", tex.getDebugName(), packed.bytes, buf._debugName, buf._bufferSize);
		ASSERT_PASS(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, pass, "TransferBuilder::readback: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);
		vkCmdCopyImageToBuffer(cmd._wrapper->_cmdBuf, tex.getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf._vkBuffer, 1, &packed.region);
	}
	void RenderGraph::RecordImageUpload(CommandBuffer& cmd, const CompiledPass& pass, const ImageUploadOp& op) const {
		const AllocatedTexture& tex = cmd._ctx->view(ResolveHandle(_textures, *cmd._ctx, op.dst.texture));
		PackedMip packed = PackMip(tex, op.dst, pass);
		ASSERT_PASS(op.bytes.size() == packed.bytes, pass, "TransferBuilder::upload: '{}' needs {} bytes for the mip, got {}.", tex.getDebugName(), packed.bytes, op.bytes.size());
		ASSERT_PASS(tex._vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT, pass, "TransferBuilder::upload: texture '{}' was not created with TRANSFER_DST usage.", tex.getDebugName());
		packed.region.bufferOffset = cmd._ctx->_staging->stageContiguous(op.bytes.data(), op.bytes.size());
		vkCmdCopyBufferToImage(cmd._wrapper->_cmdBuf, cmd._ctx->_staging->getStagingVkBuffer(), tex.getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &packed.region);
	}

	// enters with mip 0 in TRANSFER_SRC and the rest in TRANSFER_DST, and leaves them that way so the trackers stay right
	void RenderGraph::RecordGenerateMips(VkCommandBuffer cmd, const CompiledPass& pass, const AllocatedTexture& tex) {
		constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		const VkFormatFeatureFlags features = tex._vkFormatProperties.optimalTilingFeatures;
		ASSERT_PASS((features & kBlit) == kBlit, pass, "TransferBuilder::generateMipmaps: format {} of '{}' cannot be blitted.", string_VkFormat(tex.getFormat()), tex.getDebugName());
		const bool linear = !vkutil::IsFormatDepthOrStencil(tex.getFormat()) && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
		const VkImageAspectFlags aspect = vkutil::AspectMaskFromFormat(tex.getFormat());
		const uint32_t numMips = tex.getNumMips();
		const uint32_t numLayers = tex.getNumLayers();
		const Dimensions full = tex.getDimensions();
		constexpr vkutil::StageAccess kWrite = { .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT };
		constexpr vkutil::StageAccess kRead = { .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT };
		const auto extentOf = [&](const uint32_t mip) {
			return VkOffset3D{ std::max(1, static_cast<int32_t>(full.width >> mip)), std::max(1, static_cast<int32_t>(full.height >> mip)), std::max(1, static_cast<int32_t>(full.depth >> mip)) };
		};
		for (uint32_t i = 1; i < numMips; ++i) {
			const VkImageBlit blit = {
				.srcSubresource = { aspect, i - 1, 0, numLayers },
				.srcOffsets = { { 0, 0, 0 }, extentOf(i - 1) },
				.dstSubresource = { aspect, i, 0, numLayers },
				.dstOffsets = { { 0, 0, 0 }, extentOf(i) },
			};
			vkCmdBlitImage(cmd, tex.getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
			if (i + 1 < numMips)
				vkutil::ImageMemoryBarrier2(cmd, tex.getImage(), kWrite, kRead, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, { aspect, i, 1, 0, numLayers });
		}
		if (numMips > 2)
			vkutil::ImageMemoryBarrier2(cmd, tex.getImage(), kRead, kWrite, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, { aspect, 1, numMips - 2, 0, numLayers });
	}

	void RenderGraph::RecordTransferOps(CommandBuffer& cmd, const CompiledPass& pass) const {
		for (const TransferOp& op: pass.transferOps) {
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
				const std::span<const std::byte> data = update->bytes;
				AllocatedBuffer& buf = cmd._ctx->access(update->dst);
				ASSERT_PASS(update->dstOffset + data.size() <= buf._bufferSize, pass, "TransferBuilder::update: offset + size exceeds buffer '{}' size.", buf._debugName);
				ASSERT_PASS(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, pass, "TransferBuilder::update: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);

				vkCmdUpdateBuffer(cmd._wrapper->_cmdBuf, buf._vkBuffer, update->dstOffset, data.size(), data.data());
			} else if (const auto* upload = std::get_if<UploadOp>(&op)) {
				const std::span<const std::byte> data = upload->bytes;
				AllocatedBuffer& buf = cmd._ctx->access(upload->dst);
				ASSERT_PASS(upload->dstOffset + data.size() <= buf._bufferSize, pass, "TransferBuilder::upload: offset + size exceeds buffer '{}' size.", buf._debugName);
				ASSERT_PASS(
					buf.isMapped() || (buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), pass, "TransferBuilder::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);
				if (buf.isMapped()) {
					buf.bufferSubData(*cmd._ctx, upload->dstOffset, data.size(), data.data());
					continue;
				}
				auto copies = cmd._ctx->_staging->stageBufferCopy(buf, data.data(), data.size(), upload->dstOffset);
				cmd._ctx->_staging->recordBufferCopies(cmd._wrapper->_cmdBuf, copies);
			} else if (const auto* bufferCopy = std::get_if<BufferCopyOp>(&op)) {
				const AllocatedBuffer& src = cmd._ctx->view(bufferCopy->src);
				const AllocatedBuffer& dst = cmd._ctx->view(bufferCopy->dst);
				ASSERT_PASS(src._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pass, "TransferBuilder::copy: buffer '{}' was not created with TRANSFER_SRC usage.", src._debugName);
				ASSERT_PASS(dst._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, pass, "TransferBuilder::copy: buffer '{}' was not created with TRANSFER_DST usage.", dst._debugName);
				ASSERT_PASS(src._bufferSize <= dst._bufferSize, pass, "TransferBuilder::copy: '{}' ({} bytes) does not fit in '{}' ({} bytes).", src._debugName, src._bufferSize, dst._debugName, dst._bufferSize);
				const VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = src._bufferSize };
				vkCmdCopyBuffer(cmd._wrapper->_cmdBuf, src._vkBuffer, dst._vkBuffer, 1, &region);
			} else if (const auto* readback = std::get_if<ReadbackOp>(&op)) {
				RecordReadback(cmd, pass, *readback);
			} else if (const auto* imageUpload = std::get_if<ImageUploadOp>(&op)) {
				RecordImageUpload(cmd, pass, *imageUpload);
			} else if (const auto* mips = std::get_if<GenerateMipsOp>(&op)) {
				RecordGenerateMips(cmd._wrapper->_cmdBuf, pass, cmd._ctx->view(ResolveHandle(_textures, *cmd._ctx, mips->texture)));
			} else if (const auto* build = std::get_if<AccelerationStructureBuildOp>(&op)) {
				const AllocatedAccelerationStructure* structure = cmd._ctx->_accelerationStructurePool.get(build->structure);
				ASSERT_PASS(structure, pass, "builds an acceleration structure that was destroyed.");
				cmd._ctx->recordAccelerationStructureBuild(cmd._wrapper->_cmdBuf, *structure, build->refit);
			}
		}
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
		return { vkutil::AspectMaskFromFormat(fmt), range.baseMip, range.numMips, range.baseLayer, range.numLayers };
	}
	TextureShape RenderGraph::shapeOf(const CTX& rCtx, const GraphTexture texture) const {
		if (const auto* declared = std::get_if<DeclaredTexture>(&ResourceOf(_textures, texture).source)) {
			const GraphTextureSpec& spec = declared->spec;
			return {
				.debugName = spec.debugName,
				.format = spec.format,
				.sampleCount = static_cast<uint8_t>(toVulkan(spec.samples)),
				.dimensions = sizeOf(rCtx, spec),
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
			shape.numMips);
		ASSERT_PASS(baseLayer < shape.numLayers, pass, "Texture '{}' base layer {} is outside the texture's {} layers", shape.debugName, baseLayer, shape.numLayers);
		ASSERT_PASS(
			numLayers > 0 && baseLayer + numLayers <= shape.numLayers,
			pass,
			"Texture '{}' layer range [{}, {}) is outside the texture's {} layers",
			shape.debugName,
			baseLayer,
			baseLayer + numLayers,
			shape.numLayers);

		return { baseMip, numMips, baseLayer, numLayers };
	}

	static AttachmentTarget PlanAttachmentTarget(const TextureDesc& texDesc, const TextureShape& shape, const PassDesc& pass) {
		return {
			.resource = BaseOf(texDesc.texture),
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
		UNREACHABLE("Unsupported TextureAccess value.");
	}
	static vkutil::StageAccess GetTextureStageAccess(TextureAccess access, PassDesc::Type passType) {
		const VkPipelineStageFlags2 shaderStages = vkutil::GetPassShaderStages(passType);
		// SHADER_READ covers bindless handles that end up sampled rather than loaded
		switch (access) {
			case TextureAccess::Sampled:
			case TextureAccess::StorageRead:
				return { .stage = shaderStages, .access = VK_ACCESS_2_SHADER_READ_BIT };
			case TextureAccess::StorageWrite:
				return { .stage = shaderStages, .access = VK_ACCESS_2_SHADER_WRITE_BIT };
			case TextureAccess::StorageReadWrite:
				return { .stage = shaderStages, .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT };
			case TextureAccess::TransferRead:
				return { .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT };
			case TextureAccess::TransferWrite:
				return { .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT };
		}
		UNREACHABLE("Unsupported TextureAccess value.");
	}
	void RenderGraph::planResourceAccess(const CTX& rCtx, const TextureDesc& texDesc, VkImageLayout desiredLayout, const StageAccess& dstMask, CompiledPass& outPass, const PassDesc& pass, const bool keepsContents) const {
		const SubresourceRange range = ResolveSubresourceRange(texDesc, shapeOf(rCtx, texDesc.texture), pass);
		const bool firstDeclaredVersion = VersionOf(texDesc.texture) == 0 && std::holds_alternative<DeclaredTexture>(ResourceOf(_textures, texDesc.texture).source);
		outPass.imageBarriers.push_back({ .resource = BaseOf(texDesc.texture), .range = range, .dstLayout = desiredLayout, .dstMask = dstMask, .isSwapchain = IsBackBuffer(_textures, texDesc.texture), .keepsContents = keepsContents && !firstDeclaredVersion });
	}

	void RenderGraph::planPassResources(const CTX& rCtx, const PassDesc& passDesc, CompiledPass& outPass) const {
		outPass.imageBarriers.reserve(passDesc.textureDependencyOperations.size() + passDesc.attachmentOperations.size());
		outPass.bufferBarriers.reserve(passDesc.bufferDependencyOperations.size());
		for (const TextureDependencyDesc& dependency_desc: passDesc.textureDependencyOperations) {
			planResourceAccess(rCtx, dependency_desc.texDesc, GetTextureLayout(dependency_desc.access), GetTextureStageAccess(dependency_desc.access, passDesc.type), outPass, passDesc, true);
		}
		for (const AttachmentDesc& attachment_desc: passDesc.attachmentOperations) {
			const TextureShape shape = shapeOf(rCtx, attachment_desc.texDesc.texture);
			const VkImageLayout layout = vkutil::IsFormatDepth(shape.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			planResourceAccess(rCtx, attachment_desc.texDesc, layout, vkutil::GetPipelineStageAccess(layout), outPass, passDesc, attachment_desc.loadOp == LoadOp::LOAD);

			if (attachment_desc.resolveTexDesc.has_value()) {
				// resolves always write in color attachment output, even for depth/stencil
				constexpr StageAccess resolveMask = { .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
				planResourceAccess(rCtx, attachment_desc.resolveTexDesc.value(), layout, resolveMask, outPass, passDesc, false);
			}
		}
		for (const BufferDependencyDesc& dependency_desc: passDesc.bufferDependencyOperations)
			AddBufferBarrier(outPass.bufferBarriers, dependency_desc.handle, vkutil::GetBufferStageAccess(dependency_desc.access, passDesc.type));
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
					resolve_shape.debugName);
				ASSERT_PASS(shape.sampleCount > VK_SAMPLE_COUNT_1_BIT, pass_desc, "Resolve operation on non-multisampled texture '{}'!", shape.debugName);
				ASSERT_PASS(
					resolve_shape.format == shape.format,
					pass_desc,
					"Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
					shape.debugName,
					string_VkFormat(shape.format),
					resolve_shape.debugName,
					string_VkFormat(resolve_shape.format));
#ifdef DEBUG
				if (attachment_desc.storeOp == StoreOp::STORE && !_hasReportedCompile)
					LOG_PASS(
						LogType::Suggestion,
						pass_desc,
						"Attachment of texture '{}' has StoreOp::STORE and resolves into '{}', can be replaced with StoreOp::DONT_CARE.",
						shape.debugName,
						resolve_shape.debugName);
#endif

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

#ifdef DEBUG
			if ((max_width != width || max_height != height) && (max_width != 0 || max_height != 0) && !_hasReportedCompile)
				LOG_PASS(LogType::Warning, pass_desc, "You have attachments of different dimensions, this is allowed but experimental.");
#endif
			max_width = std::max(max_width, width);
			max_height = std::max(max_height, height);
		}
		outPass.renderArea = { { 0, 0 }, { max_width, max_height } };
	}

	void RenderGraph::bindPass(CTX& rCtx, CompiledPass& pass) const {
		for (CompiledImageBarrier& barrier: pass.imageBarriers) {
			if (!barrier.isSwapchain)
				barrier.handle = ResolveHandle(_textures, rCtx, barrier.resource);
		}
		for (CompiledImageBarrier& barrier: pass.exitBarriers) {
			if (!barrier.isSwapchain)
				barrier.handle = ResolveHandle(_textures, rCtx, barrier.resource);
		}
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

	TextureHandle TransientPool::acquire(CTX& ctx, const TransientKey& key, const TextureHandle previous, const TextureSpec& spec) {
		Entry* match = nullptr;
		for (Entry& e: entries) {
			if (e.taken || e.key != key)
				continue;
			match = &e;
			if (e.texture.handle() == previous)
				break;
		}
		if (!match) {
			match = &entries.emplace_back(Entry{ key, ctx.createTexture(spec) });
		}
		match->taken = true;
		match->lastUsedFrame = ctx.getCurrentFrameNumber();
		return match->texture.handle();
	}

	void RenderGraph::realizeDeclared(CTX& rCtx, const std::vector<uint32_t>& culledOrder, const std::vector<bool>& confined) {
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
		_transientPool.beginCompile();
		for (uint32_t id = 0; id < _textures.size(); ++id) {
			auto* declared = std::get_if<DeclaredTexture>(&_textures[id].source);
			if (!declared || !uses[id].has_value())
				continue;
			Realization wanted = InferRealization(sizeOf(rCtx, declared->spec), *uses[id], confined[id]);
			wanted.concurrent = declared->concurrent;
			ASSERT_MSG(wanted.usage != 0, "Graph texture '{}' is only touched by transfers, nothing samples, stores to or renders into it.", declared->spec.debugName);
			declared->texture = _transientPool.acquire(rCtx, KeyOf(declared->spec, wanted), declared->texture, ToTextureSpec(declared->spec, wanted));
			declared->realized = wanted;
		}
		_transientPool.evictIdle(rCtx.getCurrentFrameNumber());
	}


	// resolves run at EndRendering, so a later member may not keep drawing into an attachment the group already resolves
	static const char* AttachmentBlocker(const AttachmentInfo& group, const AttachmentInfo& next) {
		if (group.target.resource != next.target.resource || group.target.range != next.target.range)
			return "a different attachment image or subresource";
		if (next.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			return "LoadOp::CLEAR on an attachment the earlier pass already writes";
		if (group.resolveTarget.has_value())
			return "the earlier pass resolves an attachment the later one keeps drawing into";
		return nullptr;
	}

	struct PassResourceUse {
		std::unordered_set<GraphTexture, GraphTextureHash> attachments;
		std::unordered_map<GraphTexture, bool, GraphTextureHash> textures;
		std::unordered_map<BufferHandle, bool> buffers;

		void add(const PassDesc& desc) {
			for (const AttachmentDesc& attachment: desc.attachmentOperations) {
				attachments.insert(BaseOf(attachment.texDesc.texture));
				if (attachment.resolveTexDesc.has_value())
					attachments.insert(BaseOf(attachment.resolveTexDesc->texture));
			}
			for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
				textures[BaseOf(dependency.texDesc.texture)] |= isWrite(dependency.access);
			for (const BufferDependencyDesc& dependency: desc.bufferDependencyOperations)
				buffers[dependency.handle] |= isWrite(dependency.access);
		}
	};

	static std::vector<GraphTexture> SortedDeclaredTextures(const PassDesc& desc) {
		std::vector<GraphTexture> declared;
		for (const AttachmentDesc& attachment: desc.attachmentOperations) {
			declared.push_back(BaseOf(attachment.texDesc.texture));
			if (attachment.resolveTexDesc.has_value())
				declared.push_back(BaseOf(attachment.resolveTexDesc->texture));
		}
		for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
			declared.push_back(BaseOf(dependency.texDesc.texture));
		std::ranges::sort(declared, {}, IdOf);
		const auto [first, last] = std::ranges::unique(declared);
		declared.erase(first, last);
		return declared;
	}

	// any of these would need a barrier between members, which a single rendering scope cannot hold
	static bool HasMemberHazard(const PassResourceUse& group, const PassResourceUse& next) {
		const auto writeConflict = [](const auto& a, const auto& b) {
			return std::ranges::any_of(a, [&b](const auto& entry) {
				const auto it = b.find(entry.first);
				return it != b.end() && (entry.second || it->second);
			});
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
	};

	// the setting that keeps next out of group's rendering scope, nullptr when it can merge
	static const char* MergeBlocker(const CompilingPass& groupRecord, const CompilingPass& nextRecord) {
		const CompiledPass& group = groupRecord.pass;
		const CompiledPass& next = nextRecord.pass;
		const bool hasAttachments = !group.colorAttachments.empty() || group.depthAttachment.has_value();
		if (!hasAttachments || group.type != PassDesc::Type::Graphics || next.type != group.type)
			return "not two graphics passes with attachments";
		if (next.queue != group.queue)
			return "a different queue";
		if (group.inOrderBarrier || next.inOrderBarrier)
			return "executeCmdsInOrder()";
		if (next.layerCount != group.layerCount || next.viewMask != group.viewMask)
			return "multiview() layer count or view mask";
		if (next.renderArea.offset.x != group.renderArea.offset.x || next.renderArea.offset.y != group.renderArea.offset.y ||
			next.renderArea.extent.width != group.renderArea.extent.width || next.renderArea.extent.height != group.renderArea.extent.height)
			return "a different render area";
		if (next.colorAttachments.size() != group.colorAttachments.size())
			return "a different number of color attachments";
		// pipelines that bring their own formats (ImGui) cannot draw with an attachment they never declared
		if (next.depthAttachment.has_value() != group.depthAttachment.has_value())
			return kDepthOnOneBlocker;
		for (size_t i = 0; i < group.colorAttachments.size(); ++i) {
			if (const char* blocker = AttachmentBlocker(group.colorAttachments[i], next.colorAttachments[i]))
				return blocker;
		}
		if (group.depthAttachment.has_value()) {
			if (const char* blocker = AttachmentBlocker(*group.depthAttachment, *next.depthAttachment))
				return blocker;
		}
		if (HasMemberHazard(groupRecord.use, nextRecord.use))
			return "one reads or writes what the other writes outside its attachments";
		for (const CompiledImageBarrier& request: next.imageBarriers) {
			const auto it = std::ranges::find_if(group.imageBarriers, [&](const CompiledImageBarrier& existing) { return existing.resource == request.resource; });
			if (it != group.imageBarriers.end() && (it->range != request.range || it->dstLayout != request.dstLayout))
				return "a shared texture in a different layout or subresource";
		}
		return nullptr;
	}

	// leaves both records untouched when it returns false, the scheduler tries candidates on that assumption
	static bool TryMergeInto(CompilingPass& groupRecord, CompilingPass& nextRecord) {
		if (MergeBlocker(groupRecord, nextRecord))
			return false;
		CompiledPass& group = groupRecord.pass;
		CompiledPass& next = nextRecord.pass;

		const auto findImageBarrier = [&](const CompiledImageBarrier& request) {
			return std::ranges::find_if(group.imageBarriers, [&](const CompiledImageBarrier& existing) { return existing.resource == request.resource; });
		};
		for (const CompiledImageBarrier& request: next.imageBarriers) {
			if (const auto it = findImageBarrier(request); it != group.imageBarriers.end()) {
				it->dstMask.stage |= request.dstMask.stage;
				it->dstMask.access |= request.dstMask.access;
			} else {
				group.imageBarriers.push_back(request);
			}
		}
		for (const CompiledBufferBarrier& request: next.bufferBarriers)
			AddBufferBarrier(group.bufferBarriers, request.handle, request.dstMask);
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
		groupRecord.pass.descIndices.insert(groupRecord.pass.descIndices.end(), nextRecord.pass.descIndices.begin(), nextRecord.pass.descIndices.end());
		return true;
	}

	static bool WritesOutsideGraph(const PassDesc& desc, const std::vector<GraphResource>& textures) {
		const auto outside = [&](const GraphTexture texture) { return !std::holds_alternative<DeclaredTexture>(ResourceOf(textures, texture).source); };
		for (const AttachmentDesc& attachment: desc.attachmentOperations) {
			if (outside(attachment.texDesc.texture) || (attachment.resolveTexDesc.has_value() && outside(attachment.resolveTexDesc->texture)))
				return true;
		}
		for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations) {
			if (isWrite(dependency.access) && outside(dependency.texDesc.texture))
				return true;
		}
		return std::ranges::any_of(desc.bufferDependencyOperations, [](const BufferDependencyDesc& dependency) { return isWrite(dependency.access); });
	}

	static std::unordered_set<uint32_t> RootPasses(const std::vector<PassDesc>& pass_descs, const std::vector<GraphResource>& textures) {
		std::unordered_set<uint32_t> sinkPasses;
		for (uint32_t i = 0; i < pass_descs.size(); i++) {
			if (pass_descs[i].sideEffect || WritesOutsideGraph(pass_descs[i], textures))
				sinkPasses.insert(i);
		}
		return sinkPasses;
	}

	std::vector<uint32_t> cull_unused_passes(
		const std::vector<uint32_t>& topo_sorted_order,
		const std::vector<std::unordered_set<uint32_t>>& predecessors,
		const std::vector<PassDesc>& pass_descs,
		const std::vector<GraphResource>& textures) {
		const std::unordered_set<uint32_t> sinkPasses = RootPasses(pass_descs, textures);
		// now reverse BFS
		std::unordered_set<uint32_t> reachablePasses;
		std::queue<uint32_t> worklist;
		for (uint32_t sink: sinkPasses) {
			worklist.push(sink);
			reachablePasses.insert(sink);
		}
		while (!worklist.empty()) {
			const uint32_t node = worklist.front();
			worklist.pop();
			for (uint32_t pred: predecessors[node]) {
				if (reachablePasses.insert(pred).second)
					worklist.push(pred);
			}
		}
		// filter sortedOrder while preserving order
		std::vector<uint32_t> result;
		result.reserve(reachablePasses.size());
		for (uint32_t passIdx: topo_sorted_order) {
			if (reachablePasses.contains(passIdx))
				result.push_back(passIdx);
		}
		return result;
	}

	static bool UsesBackBuffer(const CompiledPass& pass) {
		return std::ranges::any_of(pass.imageBarriers, &CompiledImageBarrier::isSwapchain);
	}

	template<typename Op>
	static bool HasOp(const PassDesc& pass) {
		return std::ranges::any_of(pass.transferOps, [](const TransferOp& op) { return std::holds_alternative<Op>(op); });
	}
	QueueNeed QueueNeedOf(const PassDesc& pass, const std::vector<GraphResource>& textures) {
		// a build records on a compute capable queue, the transfer queue cannot run it
		const PassDesc::Type type = pass.type == PassDesc::Type::Transfer && HasOp<AccelerationStructureBuildOp>(pass) ? PassDesc::Type::Compute : pass.type;
		if (pass.type == PassDesc::Type::Graphics)
			return { type, "a graphics pass" };
		if (HasOp<BlitOp>(pass))
			return { type, "a blit" };
		if (HasOp<GenerateMipsOp>(pass))
			return { type, "mip generation" };
		if (std::ranges::any_of(pass.textureDependencyOperations, [&](const TextureDependencyDesc& dependency) { return IsBackBuffer(textures, dependency.texDesc.texture); }))
			return { type, "the back buffer" };
		return { type };
	}

	void RenderGraph::compile(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		this->_compiledPasses.clear();
		std::erase_if(_resourceTrackers, [&rCtx](const auto& entry) { return rCtx._texturePool.get(entry.first) == nullptr; });
		std::erase_if(_bufferTrackers, [&rCtx](const auto& entry) { return rCtx._bufferPool.get(entry.first) == nullptr; });
		for (const GraphResource& resource: _textures) {
			if (const auto* imported = std::get_if<ImportedTexture>(&resource.source))
				ASSERT_MSG(rCtx._texturePool.get(imported->handle) != nullptr, "A texture imported into the RenderGraph was destroyed while the graph still uses it.");
		}

		std::vector<PassUses> passUses;
		passUses.reserve(_passDescriptions.size());
		for (const PassDesc& desc: _passDescriptions)
			passUses.push_back(UsesOf(desc));
		const PassEdges edges = BuildPassEdges(passUses, _textures.size(), _buffers.size());
		std::vector<uint32_t> declaration_order(this->_passDescriptions.size());
		std::iota(declaration_order.begin(), declaration_order.end(), 0u);
		const std::vector<uint32_t> culled_order = _options.cull ? cull_unused_passes(declaration_order, edges.data, _passDescriptions, _textures) : declaration_order;
#ifdef DEBUG
		std::unordered_set<std::string> findings;
		const auto isNew = [&](const std::string& key) { return findings.insert(key).second && !_findings.contains(key); };
		reportDeclarations(culled_order, passUses, findings);
#endif

		const auto usable = [&](const std::unique_ptr<ImmediateCommands>& imm) { return imm && _options.async; };
		const AsyncQueues available{ .compute = usable(rCtx._immAsyncCompute), .transfer = usable(rCtx._immAsyncTransfer) };
#ifdef DEBUG
		const bool logRouting = !this->_hasReportedCompile && _options.async;
#endif
		const auto queueOf = [&](const PassDesc& pass) {
			const QueueNeed need = QueueNeedOf(pass, _textures);
			ASSERT_PASS(!pass.async || !need.graphicsReason, pass, "is async() but uses {}, which needs the graphics queue. Remove async() or move that work to another pass.", need.graphicsReason);
			const QueueAffinity queue = QueueOf(need.type, pass.async, available);
#ifdef DEBUG
			if (pass.async && logRouting) {
				if (queue == QueueAffinity::Graphics)
					LOG_PASS(LogType::Warning, pass, "Demoted to graphics: No async {}family available.", need.type == PassDesc::Type::Transfer ? "" : "compute ");
				else if (need.type == PassDesc::Type::Transfer && queue == QueueAffinity::AsyncCompute)
					LOG_PASS(LogType::Info, pass, "Routed to async compute: No dedicated transfer family.");
			}
#endif
			return queue;
		};

		std::vector<CompilingPass> records;
		records.reserve(culled_order.size());
		constexpr uint32_t kUnscheduled = UINT32_MAX;
		std::vector<uint32_t> descToNode(this->_passDescriptions.size(), kUnscheduled);
		for (const uint32_t pass_idx: culled_order) {
			const PassDesc& pass_desc = this->_passDescriptions[pass_idx];
			descToNode[pass_idx] = static_cast<uint32_t>(records.size());
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.queue = queueOf(pass_desc);
			compiled_pass.type = pass_desc.type;
			if (pass_desc.execute) {
				compiled_pass.members.emplace_back(pass_desc.name, pass_desc.execute, SortedDeclaredTextures(pass_desc), pass_desc.textureDependencyOperations, pass_desc.bufferDependencyOperations);
			}
			compiled_pass.transferOps = pass_desc.transferOps;
			compiled_pass.layerCount = pass_desc.layerCount;
			compiled_pass.viewMask = pass_desc.viewMask;

			planPassResources(rCtx, pass_desc, compiled_pass);
			planAttachments(rCtx, pass_desc, compiled_pass);
			// execute submits this frame with the timeline value frame number + 1
			for (const BufferDependencyDesc& dependency: pass_desc.bufferDependencyOperations)
				rCtx.access(dependency.handle)._graphTimelineValue = rCtx.getCurrentFrameNumber() + 1;
			if (pass_desc.executeInOrder)
				compiled_pass.inOrderBarrier = BuildInOrderBarrier(compiled_pass);

			CompilingPass& record = records.emplace_back(std::move(compiled_pass));
			record.use.add(pass_desc);
			record.pass.descIndices.push_back(pass_idx);
		}

		std::vector<ScheduleNode> nodes(records.size());
		for (uint32_t node = 0; node < records.size(); ++node) {
			const uint32_t pass_idx = culled_order[node];
			for (const uint32_t pred: edges.order[pass_idx]) {
				if (descToNode[pred] != kUnscheduled)
					nodes[node].predecessors.push_back(descToNode[pred]);
			}
			const CompiledPass& pass = records[node].pass;
			nodes[node].async = pass.queue != QueueAffinity::Graphics;
			nodes[node].renders = !pass.colorAttachments.empty() || pass.depthAttachment.has_value();
		}
		for (uint32_t node = 0; node < records.size(); ++node) {
			for (const uint32_t pred: edges.data[culled_order[node]]) {
				if (descToNode[pred] != kUnscheduled && nodes[descToNode[pred]].async)
					nodes[node].readsAsync = true;
			}
		}
		const ScheduleResult scheduled = SchedulePasses(std::span<const ScheduleNode>(nodes), _options.declaredOrder, _options.merge, [&](const std::span<const uint32_t> group, const uint32_t candidate) {
			return TryMergeInto(records[group.front()], records[candidate]);
		});
#ifdef DEBUG
		if (!scheduled.blocked.empty()) {
			const PassDesc& blocked = _passDescriptions[culled_order[scheduled.blocked.front()]];
			std::string names;
			for (const uint32_t node: scheduled.blocked)
				names += (names.empty() ? "'" : ", '") + _passDescriptions[culled_order[node]].name + "'";
			ASSERT_PASS(!_options.declaredOrder, blocked, "is declared before a pass it depends on, so Options::declaredOrder cannot schedule it. Declare it after its dependencies.");
			ASSERT_PASS(false, blocked, "is in a dependency cycle with {}. A pass reads an old version that a later pass's input depends on, copy it or reorder the writes.", names);
		}
#endif
		ASSERT_MSG(scheduled.blocked.empty(), "RenderGraph could not schedule every pass.");

#ifdef DEBUG
		const bool logMerges = !this->_hasReportedCompile;
#endif
		std::vector<uint32_t> descToCompiled(this->_passDescriptions.size(), kUnscheduled);
		this->_compiledPasses.reserve(scheduled.groups.size());
#ifdef DEBUG
		_depthBlockedMerges.clear();
		for (size_t g = 0; g + 1 < scheduled.groups.size() && _options.merge; ++g) {
			const CompilingPass& group = records[scheduled.groups[g].front()];
			const CompilingPass& next = records[scheduled.groups[g + 1].front()];
			const bool sharesAttachment = std::ranges::any_of(next.use.attachments, [&](const GraphTexture texture) { return group.use.attachments.contains(texture); });
			if (!sharesAttachment || group.pass.type != PassDesc::Type::Graphics || next.pass.type != PassDesc::Type::Graphics)
				continue;
			const char* blocker = MergeBlocker(group, next);
			if (blocker == kDepthOnOneBlocker)
				_depthBlockedMerges.push_back(static_cast<uint32_t>(g));
			else if (blocker && isNew("merge blocked " + group.pass.name + " " + next.pass.name))
				LOG_PASS(LogType::Suggestion, next.pass, "Runs right after '{}' and shares an attachment with it, but cannot share its rendering scope because of {}.", group.pass.name, blocker);
		}
#endif
		for (const std::vector<uint32_t>& group: scheduled.groups) {
			CompilingPass& record = records[group.front()];
			for (const uint32_t pass_idx: record.pass.descIndices)
				descToCompiled[pass_idx] = static_cast<uint32_t>(this->_compiledPasses.size());
#ifdef DEBUG
			if (logMerges && record.pass.members.size() > 1)
				LOG_PASS(LogType::Info, record.pass, "Merged {} passes into one rendering scope.", record.pass.members.size());
#endif
			this->_compiledPasses.push_back(std::move(record.pass));
		}
#ifdef DEBUG
		{
			std::unordered_map<std::string, std::vector<VkImageLayout>> layouts;
			std::vector<std::string> order;
			for (const CompiledPass& pass: this->_compiledPasses) {
				for (const CompiledImageBarrier& barrier: pass.imageBarriers) {
					if (barrier.isSwapchain)
						continue;
					const SubresourceRange& r = barrier.range;
					std::string key = std::format("'{}' mips [{}, {}) layers [{}, {})", nameOf(barrier.resource), r.baseMip, r.baseMip + r.numMips, r.baseLayer, r.baseLayer + r.numLayers);
					auto [it, inserted] = layouts.try_emplace(key);
					if (inserted)
						order.push_back(key);
					if (it->second.empty() || it->second.back() != barrier.dstLayout)
						it->second.push_back(barrier.dstLayout);
				}
			}
			for (const std::string& key: order) {
				const std::vector<VkImageLayout>& seq = layouts[key];
				for (size_t i = 2; i < seq.size(); ++i) {
					if (std::find(seq.begin(), seq.begin() + static_cast<std::ptrdiff_t>(i) - 1, seq[i]) == seq.begin() + static_cast<std::ptrdiff_t>(i) - 1)
						continue;
					std::string path;
					for (const VkImageLayout layout: seq)
						path += (path.empty() ? "" : " -> ") + std::string(string_VkImageLayout(layout));
					if (isNew("layout return " + key))
						LOG_SYSTEM_NOSOURCE(LogType::Suggestion, "Texture {} goes {} in one frame. Use one access (for example StorageRead instead of Sampled) to skip the transitions back.", key, path);
					break;
				}
			}
		}
#endif

		if (const auto presenter = std::ranges::find_if(this->_compiledPasses | std::views::reverse, UsesBackBuffer); presenter != this->_compiledPasses.rend()) {
			const GraphTexture backBuffer = std::ranges::find_if(presenter->imageBarriers, &CompiledImageBarrier::isSwapchain)->resource;
			presenter->exitBarriers.push_back({
				.resource = backBuffer,
				.range = { 0, 1, 0, 1 },
				.dstLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				.dstMask = { .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_NONE },
				.isSwapchain = true,
			});
		}
		planExternal(rCtx);
		const std::vector<bool> confined = ConfinedToOneScope(this->_compiledPasses, this->_textures.size());
		realizeDeclared(rCtx, culled_order, confined);
		for (CompiledPass& pass: this->_compiledPasses)
			bindPass(rCtx, pass);
#ifdef DEBUG
		for (const CompiledPass& pass: this->_compiledPasses) {
			const auto validate = [&](const AttachmentInfo& attachment) {
				if (attachment.isSwapchainImage)
					return;
				const AllocatedTexture& texture = rCtx.view(attachment.texture);
				ASSERT_PASS(!texture.isMemoryless() || confined[IdOf(attachment.target.resource)], pass, "Memoryless attachment '{}' must be used by a single rendering scope that clears or discards it on load and discards it on store.", texture.getDebugName());
			};
			for (const AttachmentInfo& color: pass.colorAttachments)
				validate(color);
			if (pass.depthAttachment.has_value())
				validate(*pass.depthAttachment);
		}
#endif

		std::vector<std::vector<uint32_t>> compiledPredecessors(this->_compiledPasses.size());
		std::vector<QueueAffinity> queues;
		queues.reserve(this->_compiledPasses.size());
		for (uint32_t compiled_idx = 0; compiled_idx < this->_compiledPasses.size(); ++compiled_idx) {
			queues.push_back(this->_compiledPasses[compiled_idx].queue);
			std::vector<uint32_t>& preds = compiledPredecessors[compiled_idx];
			for (const uint32_t pass_idx: this->_compiledPasses[compiled_idx].descIndices) {
				for (const uint32_t pred: edges.order[pass_idx]) {
					const uint32_t compiled_pred = descToCompiled[pred];
					if (compiled_pred != kUnscheduled && compiled_pred != compiled_idx && std::ranges::find(preds, compiled_pred) == preds.end())
						preds.push_back(compiled_pred);
				}
			}
		}
		const std::vector<BatchSlot> batches = SplitBatches(queues, compiledPredecessors);
		this->_scheduleView.clear();
		for (uint32_t compiled_idx = 0; compiled_idx < this->_compiledPasses.size(); ++compiled_idx) {
			CompiledPass& pass = this->_compiledPasses[compiled_idx];
			pass.batch = batches[compiled_idx].batch;
			pass.batchWaits = batches[compiled_idx].waits;
			for (const uint32_t pass_idx: pass.descIndices)
				this->_scheduleView.push_back({ .name = _passDescriptions[pass_idx].name, .type = _passDescriptions[pass_idx].type, .queue = pass.queue, .batch = pass.batch, .mergeGroup = compiled_idx });
		}
		for (uint32_t pass_idx = 0; pass_idx < _passDescriptions.size(); ++pass_idx) {
			const PassDesc& desc = _passDescriptions[pass_idx];
			if (descToCompiled[pass_idx] == kUnscheduled)
				this->_scheduleView.push_back({ .name = desc.name, .type = desc.type, .queue = QueueOf(QueueNeedOf(desc, _textures).type, desc.async, available), .culled = true });
		}
		if (planOwnership(rCtx)) {
			realizeDeclared(rCtx, culled_order, confined);
			for (CompiledPass& pass: this->_compiledPasses)
				bindPass(rCtx, pass);
			const bool replanned = planOwnership(rCtx);
			ASSERT_MSG(!replanned, "A concurrent graph texture still needed an ownership transfer.");
		}
#ifdef DEBUG
		for (const OwnershipTransfer& transfer: _transfers) {
			if (transfer.acquirePass == OwnershipTransfer::kFrameEnd)
				continue;
			const CompiledPass& consumer = this->_compiledPasses[transfer.acquirePass];
			const std::string_view name = nameOf(consumer.imageBarriers[transfer.acquireBarrier].resource);
			if (isNew("family change " + consumer.name + " " + std::string(name)))
				LOG_PASS(LogType::Suggestion, consumer, "Takes graph texture '{}' from another queue family and keeps its contents, which costs an ownership transfer every frame. If it does not need the old contents, write it first or use LoadOp::CLEAR or DONT_CARE.", name);
		}
		const std::vector<bool> overlaps = HasIndependentMainWork(queues, compiledPredecessors);
		for (uint32_t compiled_idx = 0; compiled_idx < this->_compiledPasses.size(); ++compiled_idx) {
			const CompiledPass& pass = this->_compiledPasses[compiled_idx];
			if (pass.queue != QueueAffinity::Graphics && !overlaps[compiled_idx] && isNew("no overlap " + pass.name))
				LOG_PASS(LogType::Suggestion, pass, "Is async() but every main pass runs before or after it, so it adds queue sync and overlaps nothing. Remove async(), or give main work that does not depend on it.");
		}
		for (uint32_t compiled_idx = 0; compiled_idx < this->_compiledPasses.size(); ++compiled_idx) {
			const CompiledPass& consumer = this->_compiledPasses[compiled_idx];
			if (consumer.queue != QueueAffinity::Graphics || consumer.batch < 2 || !isNew("main split"))
				continue;
			for (size_t r = 1; r < kQueueCount; ++r) {
				if (consumer.batchWaits[r] == BatchSlot::kNone)
					continue;
				const auto producer = std::ranges::find_if(this->_compiledPasses | std::views::take(compiled_idx) | std::views::reverse, [&](const CompiledPass& p) {
					return static_cast<size_t>(p.queue) == r && p.batch == consumer.batchWaits[r];
				});
				LOG_PASS(LogType::Suggestion, consumer, "Waits on async pass '{}' and splits the main queue into a third batch this frame. Each split adds a submit and a semaphore wait, move the async work earlier or its consumer later.", producer->name);
				break;
			}
			break;
		}
		_findings = std::move(findings);
		this->_hasReportedCompile = true;
#endif
		this->_hasCompiled = true;
	}

	const ImmediateCommands& RenderGraph::ImmOf(const CTX& rCtx, const QueueAffinity queue) {
		switch (queue) {
			case QueueAffinity::AsyncCompute: return *rCtx._immAsyncCompute;
			case QueueAffinity::AsyncTransfer: return *rCtx._immAsyncTransfer;
			default: return *rCtx._immGraphics;
		}
	}
	StageAccess RenderGraph::ClampToQueue(const CTX& rCtx, const QueueAffinity queue, StageAccess mask) {
		if (queue == QueueAffinity::Graphics || ImmOf(rCtx, queue).getQueueFamilyIndex() == rCtx._graphicsQueueFamilyIndex)
			return mask;
		constexpr VkPipelineStageFlags2 kTransferStages = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
		                                                  VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
		constexpr VkPipelineStageFlags2 kComputeStages = kTransferStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
		                                                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
		constexpr VkAccessFlags2 kGraphicsOnlyAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		                                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
		const VkPipelineStageFlags2 allowed = queue == QueueAffinity::AsyncTransfer ? kTransferStages : kComputeStages;
		if ((mask.stage & ~allowed) == 0)
			return mask;
		mask.stage &= allowed;
		mask.access &= ~kGraphicsOnlyAccess;
		if (queue == QueueAffinity::AsyncTransfer)
			mask.access &= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
		// a nonzero source keeps a layout transition chained after the ALL_COMMANDS semaphore wait
		if (mask.stage == 0) {
			mask.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			mask.access = VK_ACCESS_2_NONE;
		}
		return mask;
	}

	bool RenderGraph::planOwnership(const CTX& rCtx) {
		_transfers.clear();
		_frameStartReleases.clear();
		_frameEndAcquires.clear();
		const uint32_t graphicsFamily = rCtx._graphicsQueueFamilyIndex;
		const auto familyOf = [&](const CompiledPass& pass) { return ImmOf(rCtx, pass.queue).getQueueFamilyIndex(); };
		if (std::ranges::none_of(_compiledPasses, [&](const CompiledPass& pass) { return familyOf(pass) != graphicsFamily; }))
			return false;
		std::vector<OwnershipPass> passes;
		passes.reserve(_compiledPasses.size());
		for (const CompiledPass& pass: _compiledPasses) {
			OwnershipPass& owned = passes.emplace_back(OwnershipPass{ familyOf(pass), {} });
			for (uint32_t b = 0; b < pass.imageBarriers.size(); ++b) {
				const CompiledImageBarrier& barrier = pass.imageBarriers[b];
				if (!barrier.isSwapchain && !rCtx.view(barrier.handle)._concurrent)
					owned.uses.push_back({ IdOf(barrier.resource), barrier.range, b, barrier.keepsContents });
			}
		}
		OwnershipPlan plan = PlanOwnership(passes, graphicsFamily);
#ifdef DEBUG
		for (const auto& [pass, barrier]: plan.conflicts)
			ASSERT_PASS(false, _compiledPasses[pass], "uses graph texture '{}' on another queue family than an overlapping but differently shaped range of it. Declare the same mip and layer ranges on both families.", nameOf(_compiledPasses[pass].imageBarriers[barrier].resource));
#endif

		// the waits each pass's batch has, a release must be in a batch its acquire waits on
		std::vector<std::array<uint32_t, kQueueCount>> waitsOf(_compiledPasses.size());
		std::array<uint32_t, kQueueCount> batchOf{};
		batchOf.fill(BatchSlot::kNone);
		std::array<std::array<uint32_t, kQueueCount>, kQueueCount> current{};
		for (uint32_t i = 0; i < _compiledPasses.size(); ++i) {
			const CompiledPass& pass = _compiledPasses[i];
			const auto q = static_cast<size_t>(pass.queue);
			if (batchOf[q] != pass.batch) {
				batchOf[q] = pass.batch;
				current[q] = pass.batchWaits;
			}
			waitsOf[i] = current[q];
		}
		// versions order every write, so an unordered pair is two reads, which an exclusive image cannot serve on two families at once
		bool madeConcurrent = false;
		for (const OwnershipTransfer& transfer: plan.transfers) {
			if (transfer.releasePass == OwnershipTransfer::kFrameStart || transfer.acquirePass == OwnershipTransfer::kFrameEnd)
				continue;
			const CompiledPass& producer = _compiledPasses[transfer.releasePass];
			const uint32_t waited = waitsOf[transfer.acquirePass][static_cast<size_t>(producer.queue)];
			if (waited != BatchSlot::kNone && waited >= producer.batch)
				continue;
			const CompiledPass& consumer = _compiledPasses[transfer.acquirePass];
			const GraphTexture resource = consumer.imageBarriers[transfer.acquireBarrier].resource;
			auto* declared = std::get_if<DeclaredTexture>(&_textures[IdOf(resource)].source);
			ASSERT_PASS(declared != nullptr, consumer, "and '{}' read imported texture '{}' on different queue families with no order between them, and an exclusive texture belongs to one family at a time. Create it with TextureSpec::concurrent, or make one pass depend on the other.", producer.name, nameOf(resource));
			if (!declared->concurrent) {
				declared->concurrent = true;
				madeConcurrent = true;
#ifdef DEBUG
				LOG_PASS(LogType::Suggestion, consumer, "and '{}' read graph texture '{}' on different queue families with no order between them, so it is now concurrent, which can cost compression on AMD. Make one pass depend on the other to keep it exclusive.", producer.name, nameOf(resource));
#endif
			}
		}
		if (madeConcurrent)
			return true;

		for (const auto& [pass, barrier]: plan.discards)
			_compiledPasses[pass].imageBarriers[barrier].discardContents = true;
		_transfers = std::move(plan.transfers);
		for (uint32_t t = 0; t < _transfers.size(); ++t) {
			const OwnershipTransfer& transfer = _transfers[t];
			if (transfer.releasePass == OwnershipTransfer::kFrameStart)
				_frameStartReleases.push_back(t);
			else
				_compiledPasses[transfer.releasePass].releases.push_back(t);
			if (transfer.acquirePass == OwnershipTransfer::kFrameEnd)
				_frameEndAcquires.push_back(t);
			else
				_compiledPasses[transfer.acquirePass].acquires.push_back(t);
		}
		return false;
	}

	static TextureAccessCache& TrackerOf(std::unordered_map<TextureHandle, TextureAccessCache>& trackers, AllocatedTexture& texture, const TextureHandle handle) {
		auto [it, inserted] = trackers.try_emplace(handle, TextureAccessCache{ SubresourceMap<StageAccess>{ texture.getNumMips(), texture.getNumLayers() } });
		if (inserted || it->second.layoutVersion != texture.getLayoutVersion())
			it->second.masks.set(it->second.masks.wholeRange(), { .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_MEMORY_WRITE_BIT });
		return it->second;
	}

	void RenderGraph::planExternal(const CTX& rCtx) {
		_externals.clear();
		for (uint32_t i = 0; i < _textures.size(); ++i) {
			if (_textures[i].external)
				_externals.push_back({ .texture = GraphTextureAccess::make(i), .access = *_textures[i].external });
		}
		for (const GraphBufferResource& buffer: _buffers) {
			if (buffer.external)
				_externals.push_back({ .buffer = buffer.handle, .access = *buffer.external });
		}
		const uint32_t graphicsFamily = ImmOf(rCtx, QueueAffinity::Graphics).getQueueFamilyIndex();
		for (uint32_t e = 0; e < _externals.size(); ++e) {
			ExternalUse& use = _externals[e];
#ifdef DEBUG
			const std::string_view name = use.texture.valid() ? nameOf(use.texture) : std::string_view(std::ranges::find(_buffers, use.buffer, &GraphBufferResource::handle)->debugName);
#endif
			for (uint32_t c = 0; c < _compiledPasses.size(); ++c) {
				const CompiledPass& pass = _compiledPasses[c];
				const bool uses = use.texture.valid() ? std::ranges::any_of(pass.imageBarriers, [&](const CompiledImageBarrier& b) { return IdOf(b.resource) == IdOf(use.texture); })
				                                      : std::ranges::any_of(pass.bufferBarriers, [&](const CompiledBufferBarrier& b) { return b.handle == use.buffer; });
				if (!uses)
					continue;
				if (use.first == ExternalUse::kUnused)
					use.first = c;
				use.last = c;
				ASSERT_PASS(use.access.ownerFamily == VK_QUEUE_FAMILY_IGNORED || ImmOf(rCtx, pass.queue).getQueueFamilyIndex() == graphicsFamily, pass, "uses '{}', which another family owns outside the frame, on a queue outside the graphics family. Use it on main.", name);
			}
			if (use.first == ExternalUse::kUnused)
				continue;
			const bool releasesTexture = use.texture.valid() && use.access.ownerFamily != VK_QUEUE_FAMILY_IGNORED && use.access.ownerFamily != graphicsFamily;
			ASSERT_PASS(!releasesTexture || use.access.exitLayout != VK_IMAGE_LAYOUT_UNDEFINED, _compiledPasses[use.last], "gives '{}' back to family {} at the end of the frame, which needs an exitLayout.", name, use.access.ownerFamily);
			_compiledPasses[use.first].externalEntries.push_back(e);
			_compiledPasses[use.last].externalExits.push_back(e);
			if (use.texture.valid() && use.access.exitLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
				const TextureShape shape = shapeOf(rCtx, use.texture);
				_compiledPasses[use.last].exitBarriers.push_back({
					.resource = use.texture,
					.range = { 0, shape.numMips, 0, shape.numLayers },
					.dstLayout = use.access.exitLayout,
					.dstMask = { .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_NONE },
				});
			}
		}
	}

	// the wait covers memory, so the trackers only need to chain later barriers through its stage
	void RenderGraph::recordExternalEntry(CommandBuffer& cmd, ImmediateCommands& imm, const ExternalUse& use) {
		const ExternalAccess& access = use.access;
		if (access.waitSemaphore)
			imm.waitTimeline(access.waitSemaphore, access.waitValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		const bool acquires = access.ownerFamily != VK_QUEUE_FAMILY_IGNORED && access.ownerFamily != imm.getQueueFamilyIndex();
		constexpr StageAccess kAfterWait = { .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_NONE };
		if (!use.texture.valid()) {
			_bufferTrackers[use.buffer] = {};
			if (acquires) {
				const VkBufferMemoryBarrier2 barrier = {
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
					.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
					.srcQueueFamilyIndex = access.ownerFamily,
					.dstQueueFamilyIndex = imm.getQueueFamilyIndex(),
					.buffer = cmd._ctx->view(use.buffer)._vkBuffer,
					.size = VK_WHOLE_SIZE,
				};
				const VkDependencyInfo info = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier };
				vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &info);
			}
			return;
		}
		const TextureHandle handle = ResolveHandle(_textures, *cmd._ctx, use.texture);
		AllocatedTexture& texture = cmd._ctx->access(handle);
		const SubresourceRange whole = { 0, texture.getNumMips(), 0, texture.getNumLayers() };
		// an UNDEFINED entry drops the contents, so the transfer is skipped
		if (acquires && access.entryLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
			const VkImageMemoryBarrier2 barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
				.oldLayout = access.entryLayout,
				.newLayout = access.entryLayout,
				.srcQueueFamilyIndex = access.ownerFamily,
				.dstQueueFamilyIndex = imm.getQueueFamilyIndex(),
				.image = texture.getImage(),
				.subresourceRange = MakeVkRange(texture.getFormat(), whole),
			};
			const VkDependencyInfo info = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier };
			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &info);
		}
		texture.setLayout(whole, access.entryLayout);
		TextureAccessCache& cache = TrackerOf(_resourceTrackers, texture, handle);
		cache.masks.set(whole, kAfterWait);
		cache.layoutVersion = texture.getLayoutVersion();
	}

	void RenderGraph::recordExternalExit(CommandBuffer& cmd, ImmediateCommands& imm, const ExternalUse& use) const {
		const ExternalAccess& access = use.access;
		if (access.ownerFamily != VK_QUEUE_FAMILY_IGNORED && access.ownerFamily != imm.getQueueFamilyIndex()) {
			VkDependencyInfo info = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			VkBufferMemoryBarrier2 bufferBarrier{};
			VkImageMemoryBarrier2 imageBarrier{};
			if (!use.texture.valid()) {
				bufferBarrier = {
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.srcQueueFamilyIndex = imm.getQueueFamilyIndex(),
					.dstQueueFamilyIndex = access.ownerFamily,
					.buffer = cmd._ctx->view(use.buffer)._vkBuffer,
					.size = VK_WHOLE_SIZE,
				};
				info.bufferMemoryBarrierCount = 1;
				info.pBufferMemoryBarriers = &bufferBarrier;
			} else {
				const AllocatedTexture& texture = cmd._ctx->view(ResolveHandle(_textures, *cmd._ctx, use.texture));
				imageBarrier = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.oldLayout = access.exitLayout,
					.newLayout = access.exitLayout,
					.srcQueueFamilyIndex = imm.getQueueFamilyIndex(),
					.dstQueueFamilyIndex = access.ownerFamily,
					.image = texture.getImage(),
					.subresourceRange = MakeVkRange(texture.getFormat(), { 0, texture.getNumMips(), 0, texture.getNumLayers() }),
				};
				info.imageMemoryBarrierCount = 1;
				info.pImageMemoryBarriers = &imageBarrier;
			}
			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &info);
		}
		if (access.signalSemaphore)
			imm.signalSemaphore(access.signalSemaphore, access.signalValue);
	}

	void RenderGraph::recordRelease(CommandBuffer& cmd, OwnershipTransfer& transfer) {
		const bool returns = transfer.acquirePass == OwnershipTransfer::kFrameEnd;
		const CompiledImageBarrier& target = returns ? _compiledPasses[transfer.releasePass].imageBarriers[transfer.releaseBarrier] : _compiledPasses[transfer.acquirePass].imageBarriers[transfer.acquireBarrier];
		AllocatedTexture& texture = cmd._ctx->access(target.handle);
		TextureAccessCache& cache = TrackerOf(_resourceTrackers, texture, target.handle);
		StageAccess src{};
		cache.masks.forEachOverlapping(transfer.range, [&](const SubresourceRange&, const StageAccess& mask) {
			src.stage |= mask.stage;
			src.access |= mask.access;
		});
		src = ClampToQueue(*cmd._ctx, cmd._queue, src);
		transfer.recordedLayouts.clear();
		std::vector<VkImageMemoryBarrier2> barriers;
		texture._recordedLayout.forEachOverlapping(transfer.range, [&](const SubresourceRange& range, const VkImageLayout layout) {
			transfer.recordedLayouts.emplace_back(range, layout);
			barriers.push_back({
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = src.stage,
				.srcAccessMask = src.access,
				.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
				.dstAccessMask = VK_ACCESS_2_NONE,
				.oldLayout = layout,
				.newLayout = returns ? layout : target.dstLayout,
				.srcQueueFamilyIndex = transfer.srcFamily,
				.dstQueueFamilyIndex = transfer.dstFamily,
				.image = texture.getImage(),
				.subresourceRange = MakeVkRange(texture.getFormat(), range),
			});
		});
		const VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()), .pImageMemoryBarriers = barriers.data() };
		vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependency);
		if (!returns)
			texture.setLayout(transfer.range, target.dstLayout);
		cache.masks.set(transfer.range, { .stage = VK_PIPELINE_STAGE_2_NONE, .access = VK_ACCESS_2_NONE });
		cache.layoutVersion = texture.getLayoutVersion();
	}

	void RenderGraph::recordAcquire(CommandBuffer& cmd, const OwnershipTransfer& transfer) {
		const bool returns = transfer.acquirePass == OwnershipTransfer::kFrameEnd;
		const CompiledImageBarrier& target = returns ? _compiledPasses[transfer.releasePass].imageBarriers[transfer.releaseBarrier] : _compiledPasses[transfer.acquirePass].imageBarriers[transfer.acquireBarrier];
		const StageAccess dst = returns ? StageAccess{ .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_NONE } : target.dstMask;
		AllocatedTexture& texture = cmd._ctx->access(target.handle);
		std::vector<VkImageMemoryBarrier2> barriers;
		barriers.reserve(transfer.recordedLayouts.size());
		for (const auto& [range, layout]: transfer.recordedLayouts) {
			barriers.push_back({
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
				.srcAccessMask = VK_ACCESS_2_NONE,
				.dstStageMask = dst.stage,
				.dstAccessMask = dst.access,
				.oldLayout = layout,
				.newLayout = returns ? layout : target.dstLayout,
				.srcQueueFamilyIndex = transfer.srcFamily,
				.dstQueueFamilyIndex = transfer.dstFamily,
				.image = texture.getImage(),
				.subresourceRange = MakeVkRange(texture.getFormat(), range),
			});
		}
		const VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()), .pImageMemoryBarriers = barriers.data() };
		vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependency);
		TextureAccessCache& cache = TrackerOf(_resourceTrackers, texture, target.handle);
		cache.masks.set(transfer.range, dst);
		cache.layoutVersion = texture.getLayoutVersion();
	}

	void RenderGraph::PerformBarrierTransitions(CommandBuffer& cmd, const std::span<const CompiledImageBarrier> imageBarriers, const std::span<const CompiledBufferBarrier> bufferBarriers) {
		if (imageBarriers.empty() && bufferBarriers.empty())
			return;
		ASSERT(cmd._wrapper->_cmdBuf);

		// per-pass scratch lives on the stack and only spills to the heap for unusually large passes
		std::array<std::byte, 8192> scratch{};
		std::pmr::monotonic_buffer_resource arena{ scratch.data(), scratch.size() };
		std::pmr::vector<VkImageMemoryBarrier2> vkImageBarriers{ &arena };
		vkImageBarriers.reserve(imageBarriers.size());
		std::pmr::vector<VkBufferMemoryBarrier2> vkBufferBarriers{ &arena };
		vkBufferBarriers.reserve(bufferBarriers.size());
		std::pmr::vector<SubresourceMap<StageAccess>::Entry> mergedReads{ &arena };
		for (const CompiledImageBarrier& req: imageBarriers) {
			// we have to update the image associated if its a swapchain image
			// as it changes every frame
			const TextureHandle activeHandle = req.isSwapchain ? cmd._ctx->getCurrentSwapchainTexHandle() : req.handle;
			AllocatedTexture& activeTexture = cmd._ctx->access(activeHandle);
#ifdef DEBUG
			if (const auto it = _resourceTrackers.find(activeHandle); !req.isSwapchain && it != _resourceTrackers.end() && it->second.layoutVersion != activeTexture.getLayoutVersion()) {
				const std::string key = "outside write " + std::string(activeTexture.getDebugName());
				_recordFindings.insert(key);
				if (_lastRecordFindings.contains(key) && _shownRecordFindings.insert(key).second)
					LOG_SYSTEM(LogType::Suggestion, "Texture '{}' is changed outside the render graph every frame, so each frame it gets the conservative ALL_COMMANDS barrier. Do that write through the graph (TransferBuilder::upload, or a pass).", activeTexture.getDebugName());
			}
#endif
			TextureAccessCache& cache = TrackerOf(_resourceTrackers, activeTexture, activeHandle);
			activeTexture._recordedLayout.forEachOverlapping(req.range, [&](const SubresourceRange& layoutRange, VkImageLayout recordedLayout) {
				const VkImageLayout currentLayout = req.discardContents ? VK_IMAGE_LAYOUT_UNDEFINED : recordedLayout;
				cache.masks.forEachOverlapping(layoutRange, [&](const SubresourceRange& range, const StageAccess& trackedMask) {
					const StageAccess currentMask = ClampToQueue(*cmd._ctx, cmd._queue, trackedMask);
					if (!req.discardContents && !NeedsBarrier(currentLayout, currentMask, req.dstLayout, req.dstMask)) {
						// a later write must wait on every reader since the last barrier
						mergedReads.push_back({ range, { .stage = trackedMask.stage | req.dstMask.stage, .access = trackedMask.access | req.dstMask.access } });
						return;
					}
					vkImageBarriers.push_back(
						{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
							.srcStageMask = currentMask.stage,
							.srcAccessMask = currentMask.access,
							.dstStageMask = req.dstMask.stage,
							.dstAccessMask = req.dstMask.access,
							.oldLayout = currentLayout,
							.newLayout = req.dstLayout,
							.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.image = activeTexture.getImage(),
							.subresourceRange = MakeVkRange(activeTexture.getFormat(), range) });
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
			std::optional<vkutil::BufferBarrier> barrier = vkutil::TrackBufferAccess(_bufferTrackers[req.handle], req.dstMask);
			if (!barrier)
				continue;
			barrier->src = ClampToQueue(*cmd._ctx, cmd._queue, barrier->src);
			const AllocatedBuffer& buffer = cmd._ctx->view(req.handle);
			vkBufferBarriers.push_back({ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = barrier->src.stage,
				.srcAccessMask = barrier->src.access,
				.dstStageMask = barrier->dst.stage,
				.dstAccessMask = barrier->dst.access,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buffer._vkBuffer,
				.offset = 0,
				.size = VK_WHOLE_SIZE });
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

	void RenderGraph::reset() {
		_passDescriptions.clear();
		_compiledPasses.clear();
		_arena.clear();
		for (std::vector<uint32_t>& producers: _textureProducers)
			producers.clear();
		for (std::vector<uint32_t>& producers: _bufferProducers)
			producers.clear();
		for (GraphResource& texture: _textures)
			texture.external.reset();
		for (GraphBufferResource& buffer: _buffers)
			buffer.external.reset();
	}

	std::string RenderGraph::dump() const {
		ASSERT_MSG(_hasCompiled, "RenderGraph::dump needs a compile since the last reset.");
		constexpr std::array<const char*, kQueueCount> kQueueNames{ "main", "async compute", "async transfer" };
		const auto escape = [](const std::string_view text) {
			std::string out;
			for (const char c: text) {
				if (c == '"' || c == '\\')
					out += '\\';
				out += c;
			}
			return out;
		};
		const auto bufferName = [&](const BufferHandle handle) -> std::string_view {
			const auto it = std::ranges::find(_buffers, handle, &GraphBufferResource::handle);
			return it == _buffers.end() ? "buffer" : std::string_view(it->debugName);
		};
		constexpr uint32_t kCulled = UINT32_MAX;
		std::vector<uint32_t> descToCompiled(_passDescriptions.size(), kCulled);
		for (uint32_t compiled_idx = 0; compiled_idx < _compiledPasses.size(); ++compiled_idx) {
			for (const uint32_t pass_idx: _compiledPasses[compiled_idx].descIndices)
				descToCompiled[pass_idx] = compiled_idx;
		}
		const auto nodeOf = [&](const uint32_t pass_idx) {
			return descToCompiled[pass_idx] == kCulled ? std::format("culled{}", pass_idx) : std::format("pass{}", descToCompiled[pass_idx]);
		};

		std::string out = "digraph RenderGraph {\n\trankdir=LR;\n\tnode [shape=box, fontname=\"monospace\", fontsize=10];\n\tedge [fontname=\"monospace\", fontsize=9];\n";
		for (size_t q = 0; q < kQueueCount; ++q) {
			std::string nodes;
			for (uint32_t compiled_idx = 0; compiled_idx < _compiledPasses.size(); ++compiled_idx) {
				const CompiledPass& pass = _compiledPasses[compiled_idx];
				if (static_cast<size_t>(pass.queue) != q)
					continue;
				std::string label = std::format("#{} {}  (batch {})\\l", compiled_idx, escape(pass.name), pass.batch);
				if (pass.descIndices.size() > 1) {
					for (const uint32_t pass_idx: pass.descIndices)
						label += std::format("  + {}\\l", escape(_passDescriptions[pass_idx].name));
				}
				for (const CompiledImageBarrier& barrier: pass.imageBarriers)
					label += std::format("  '{}' -> {}\\l", escape(nameOf(barrier.resource)), string_VkImageLayout(barrier.dstLayout));
				for (const CompiledBufferBarrier& barrier: pass.bufferBarriers)
					label += std::format("  '{}' {}\\l", escape(bufferName(barrier.handle)), string_VkPipelineStageFlags2(barrier.dstMask.stage));
				for (const CompiledImageBarrier& barrier: pass.exitBarriers)
					label += std::format("  exit '{}' -> {}\\l", escape(nameOf(barrier.resource)), string_VkImageLayout(barrier.dstLayout));
				nodes += std::format("\t\tpass{} [label=\"{}\"];\n", compiled_idx, label);
			}
			if (!nodes.empty())
				out += std::format("\tsubgraph cluster_{} {{\n\t\tlabel=\"{}\";\n{}\t}}\n", q, kQueueNames[q], nodes);
		}
		for (uint32_t pass_idx = 0; pass_idx < _passDescriptions.size(); ++pass_idx) {
			if (descToCompiled[pass_idx] == kCulled)
				out += std::format("\tculled{} [label=\"{} (culled)\", style=dashed, color=gray, fontcolor=gray];\n", pass_idx, escape(_passDescriptions[pass_idx].name));
		}
		std::unordered_set<std::string> edges;
		for (uint32_t pass_idx = 0; pass_idx < _passDescriptions.size(); ++pass_idx) {
			const PassUses uses = UsesOf(_passDescriptions[pass_idx]);
			const auto edge = [&](const std::vector<std::vector<uint32_t>>& producers, const ResourceUse& use, const std::string_view name) {
				if (use.version == 0 || use.id >= producers.size() || use.version > producers[use.id].size())
					return;
				const uint32_t producer = producers[use.id][use.version - 1];
				const std::string from = nodeOf(producer), to = nodeOf(pass_idx);
				if (from != to)
					edges.insert(std::format("\t{} -> {} [label=\"{} v{}\"];\n", from, to, escape(name), use.version));
			};
			for (const ResourceUse& use: uses.textures)
				edge(_textureProducers, use, nameOf(GraphTextureAccess::make(use.id)));
			for (const ResourceUse& use: uses.buffers)
				edge(_bufferProducers, use, _buffers[use.id].debugName);
		}
		std::vector<std::string> sorted(edges.begin(), edges.end());
		std::ranges::sort(sorted);
		for (const std::string& line: sorted)
			out += line;
		out += "}\n";
		return out;
	}

	SubmitHandle RenderGraph::execute(CTX& rCtx, const QueryPoolHandle timestamps, const uint32_t firstQuery) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!rCtx._currentCommandBuffer._graph, "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		const bool presents = std::ranges::any_of(_compiledPasses, UsesBackBuffer);
		CommandBuffer* acquired = rCtx.acquireCommand(presents);
		// out of date, the swapchain is marked dirty and this frame is dropped
		if (!acquired)
			return rCtx._immGraphics->getLastSubmitHandle();

		std::array<bool, kQueueCount> stampsQueue{};
		if (timestamps.valid()) {
			rCtx.resetQueryPool(timestamps, firstQuery, static_cast<uint32_t>(2 * _compiledPasses.size()));
			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(rCtx._vkPhysicalDevice, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(rCtx._vkPhysicalDevice, &familyCount, families.data());
			const std::array<uint32_t, kQueueCount> familyOf{ rCtx._graphicsQueueFamilyIndex, rCtx._computeQueueFamilyIndex, rCtx._transferQueueFamilyIndex };
			for (size_t q = 0; q < kQueueCount; ++q)
				stampsQueue[q] = familyOf[q] < familyCount && families[familyOf[q]].timestampValidBits > 0;
		}

		constexpr uint32_t kNone = BatchSlot::kNone;
		const std::array<ImmediateCommands*, kQueueCount> imms{ rCtx._immGraphics.get(), rCtx._immAsyncCompute.get(), rCtx._immAsyncTransfer.get() };
		// main records into the CTX's command buffer, pipeline creation reads its active pass
		std::array<CommandBuffer, kQueueCount> asyncLanes;
		std::array<CommandBuffer*, kQueueCount> lanes{ acquired, nullptr, nullptr };
		std::array<uint32_t, kQueueCount> laneBatch{ 0, kNone, kNone };
		std::array<std::vector<uint64_t>, kQueueCount> signaled;
		const auto submitLane = [&](const size_t q) {
			CommandBuffer& lane = *lanes[q];
#ifdef MYTH_ENABLED_TRACY_GPU
			if (tracy::VkCtx* tracyCtx = rCtx._tracyPlugin.getTracyVkCtx(static_cast<QueueAffinity>(q)))
				TracyVkCollect(tracyCtx, lane._wrapper->_cmdBuf);
#endif
			imms[q]->submit(*lane._wrapper);
			signaled[q].resize(laneBatch[q] + 1, 0);
			signaled[q][laneBatch[q]] = imms[q]->getTimelineValue();
			if (q == 0) {
				lane._wrapper = &imms[0]->acquire();
				++laneBatch[0];
				return;
			}
			lanes[q] = nullptr;
			laneBatch[q] = kNone;
		};
		// images the graphics family owns from before the frame go out in their own submit, so async work does not wait on main's first batch
		uint64_t frameStartValue = 0;
		if (!_frameStartReleases.empty()) {
			for (const uint32_t t: _frameStartReleases)
				recordRelease(*lanes[0], _transfers[t]);
			imms[0]->submit(*lanes[0]->_wrapper);
			frameStartValue = imms[0]->getTimelineValue();
			lanes[0]->_wrapper = &imms[0]->acquire();
		}

		for (size_t passIdx = 0; passIdx < _compiledPasses.size(); ++passIdx) {
			CompiledPass& pass = _compiledPasses[passIdx];
			const auto q = static_cast<size_t>(pass.queue);
			for (size_t r = 0; r < kQueueCount; ++r) {
				if (pass.batchWaits[r] != kNone && lanes[r] && laneBatch[r] == pass.batchWaits[r])
					submitLane(r);
			}
			if (lanes[q] && laneBatch[q] != pass.batch)
				submitLane(q);
			if (!lanes[q]) {
				asyncLanes[q] = CommandBuffer(&rCtx, imms[q]->acquire(), pass.queue);
				lanes[q] = &asyncLanes[q];
				laneBatch[q] = pass.batch;
			}
			ASSERT_PASS(laneBatch[q] == pass.batch, pass, "records into batch {} of its queue, but the queue is on batch {}.", pass.batch, laneBatch[q]);
			for (size_t r = 0; r < kQueueCount; ++r) {
				// ALL_COMMANDS so any barrier after the wait chains with it, whatever its source stage
				if (pass.batchWaits[r] != kNone)
					imms[q]->waitTimeline(imms[r]->getTimeline(), signaled[r][pass.batchWaits[r]], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
			}
			CommandBuffer& cmd = *lanes[q];
			for (const uint32_t t: pass.acquires) {
				if (_transfers[t].releasePass == OwnershipTransfer::kFrameStart)
					imms[q]->waitTimeline(imms[0]->getTimeline(), frameStartValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
				recordAcquire(cmd, _transfers[t]);
			}
			for (const uint32_t e: pass.externalEntries)
				recordExternalEntry(cmd, *imms[q], _externals[e]);
			const bool isGraphics = pass.type == PassDesc::Type::Graphics;
			if (_options.fullBarriers) {
				constexpr VkMemoryBarrier2 kFull = {
					.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
				};
				const VkDependencyInfo full = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &kFull };
				vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &full);
			}
			// perform batched vkCmdPipelineBarrier
			PerformBarrierTransitions(cmd, pass.imageBarriers, pass.bufferBarriers);
			// keep GPU zones per pass, timestamps inside a render pass split Metal encoders on MoltenVK
			MYTH_PROFILER_GPU_ZONE(cmd._ctx, pass.queue, pass.name.c_str(), cmd._wrapper->_cmdBuf);
			const bool stamps = stampsQueue[q];
			if (stamps) cmd.cmdWriteTimestamp(timestamps, firstQuery + 2 * static_cast<uint32_t>(passIdx));
			// switch color attachment imageViews for Swapchain
			if (presents) {
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
			if (pass.type == PassDesc::Type::Transfer)
				RecordTransferOps(cmd, pass);
#ifdef DEBUG
			cmd._drewFixedFormats = false;
#endif
			for (size_t i = 0; i < pass.members.size(); ++i) {
				if (i > 0)
					cmd.resetMemberStateImpl();
				cmd._activeMember = &pass.members[i];
				pass.members[i].execute->invoke(*pass.members[i].execute, cmd);
#ifdef DEBUG
				reportObservedUses(pass, pass.members[i]);
#endif
			}
#ifdef DEBUG
			reportDepthBlockedMerges(passIdx, cmd._drewFixedFormats);
#endif
			if (isGraphics) cmd.cmdEndRenderingEXT();
			if (stamps) cmd.cmdWriteTimestamp(timestamps, firstQuery + 2 * static_cast<uint32_t>(passIdx) + 1);
			PerformBarrierTransitions(cmd, pass.exitBarriers, {});
			for (const uint32_t t: pass.releases)
				recordRelease(cmd, _transfers[t]);
			for (const uint32_t e: pass.externalExits)
				recordExternalExit(cmd, *imms[q], _externals[e]);
			cmd._graph = nullptr;
			cmd._activeMember = nullptr;
		}
		// the join, main's last batch waits on every async queue so staging, deferred tasks and pacing see one handle
		for (size_t r = 1; r < kQueueCount; ++r) {
			if (lanes[r])
				submitLane(r);
			if (!signaled[r].empty())
				imms[0]->waitTimeline(imms[r]->getTimeline(), signaled[r].back(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		}
		for (const uint32_t t: _frameEndAcquires)
			recordAcquire(*lanes[0], _transfers[t]);
		// the timeline wait does not make GPU writes visible to the host, a mapped buffer read after the frame needs this
		const bool hostReads = std::ranges::any_of(_compiledPasses, [&](const CompiledPass& pass) {
			return std::ranges::any_of(pass.bufferBarriers, [&](const CompiledBufferBarrier& b) { return (b.dstMask.access & vkutil::kWriteAccess) && rCtx.view(b.handle).isMapped(); });
		});
		if (hostReads) {
			constexpr VkMemoryBarrier2 kToHost = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
				.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
			};
			const VkDependencyInfo toHost = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &kToHost };
			vkCmdPipelineBarrier2(lanes[0]->_wrapper->_cmdBuf, &toHost);
		}
		for (const ExternalUse& use: _externals) {
			if (use.first != ExternalUse::kUnused)
				continue;
			if (use.access.waitSemaphore)
				imms[0]->waitTimeline(use.access.waitSemaphore, use.access.waitValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
			if (use.access.signalSemaphore)
				imms[0]->signalSemaphore(use.access.signalSemaphore, use.access.signalValue);
		}
#ifdef DEBUG
		_lastRecordFindings = std::move(_recordFindings);
		_recordFindings.clear();
#endif
		return rCtx.submitCommand(*lanes[0], presents);
	}
} // namespace mythril
