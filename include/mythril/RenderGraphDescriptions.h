//
// Created by Hayden Rivas on 1/20/26.
//

#pragma once

#include "Objects.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <volk.h>

// stuff declared here is read easily compared to RenderGraphInternal.h
namespace mythril {
	struct BasePassBuilder;
	class CommandBuffer;
	// possible queues used by a pass
	enum class QueueAffinity {
		Graphics,
		AsyncCompute,
		AsyncTransfer,
	};
	static constexpr int kQueueCount = 3;

	struct ClearColor {
		float r, g, b, a;
		ClearColor(float r, float g, float b, float a) :
		    r(r),
		    g(g),
		    b(b),
		    a(a) {}
		VkClearColorValue getAsVkClearColorValue() const { return {{r, g, b, a}}; }
	};

	struct ClearDepthStencil {
		float depth;
		uint32_t stencil;
		ClearDepthStencil(float depth, uint32_t stencil) :
		    depth(depth),
		    stencil(stencil) {}
		VkClearDepthStencilValue getAsVkClearDepthStencilValue() const { return {depth, stencil}; }
	};

	union ClearValue {
		ClearColor clearColor;
		ClearDepthStencil clearDepthStencil;

		// if the user sets no clear (have no intention of clearing) then do nothing ig
		ClearValue() {}
		static ClearValue color(float r, float g, float b, float a) {
			ClearValue v;
			v.clearColor = {r, g, b, a};
			return v;
		}
		static ClearValue depth(float depth, uint32_t stencil = 0) {
			ClearValue v;
			v.clearDepthStencil = {depth, stencil};
			return v;
		}
		VkClearValue getDepthStencilValue() const { return {.depthStencil = clearDepthStencil.getAsVkClearDepthStencilValue()}; }
		VkClearValue getColorValue() const { return {.color = clearColor.getAsVkClearColorValue()}; }
	};


	class RenderGraph;
	struct GraphTextureAccess;
	// a texture as one RenderGraph knows it, only the graph can mint a valid one
	class GraphTexture {
	public:
		constexpr GraphTexture() = default;
		constexpr bool valid() const { return _index != kInvalid; }
		constexpr bool operator==(const GraphTexture&) const = default;
	private:
		static constexpr uint32_t kInvalid = UINT32_MAX;
		explicit constexpr GraphTexture(uint32_t index) : _index(index) {}
		uint32_t _index = kInvalid;
		friend class RenderGraph;
		friend struct GraphTextureAccess;
	};

	class GraphExtent {
	public:
		static constexpr GraphExtent swapchain() { return GraphExtent{0}; }
		constexpr bool operator==(const GraphExtent&) const = default;
	private:
		explicit constexpr GraphExtent(uint32_t index) : _index(index) {}
		uint32_t _index = 0;
		friend class RenderGraph;
	};
	struct RelativeSize {
		GraphExtent extent = GraphExtent::swapchain();
		float scale = 1.f;
	};
	using TextureSize = std::variant<Dimensions, RelativeSize>;

	// the part of TextureSpec the graph cannot infer, usage and storage come from the passes that use it
	struct GraphTextureSpec {
		TextureSize size;
		TextureType type = TextureType::Type_2D;
		VkFormat format = VK_FORMAT_UNDEFINED;
		SampleCount samples = SampleCount::X1;
		uint32_t numMipLevels = 1;
		uint32_t numLayers = 1;
		ComponentMapping components = {};
		std::string debugName = "Unnamed Graph Texture";
	};

	// what CommandBuffer::texture returns, valid for the pass being recorded including the dry run
	struct ResolvedTexture {
		TextureHandle handle;
		Dimensions dimensions;
		uint32_t index() const { return handle.index(); }
	};

	struct TextureDesc {
		TextureDesc() = delete;
		TextureDesc(GraphTexture tex) :
		    texture(tex) {}
		GraphTexture texture;
		std::optional<uint32_t> baseLevel;
		std::optional<uint32_t> numLevels;
		std::optional<uint32_t> baseLayer;
		std::optional<uint32_t> numLayers;
		std::optional<TextureType> type;
	};
	struct AttachmentDesc {
		TextureDesc texDesc;
		ClearValue clearValue;
		LoadOp loadOp = LoadOp::DONT_CARE;
		StoreOp storeOp = StoreOp::DONT_CARE;
		std::optional<TextureDesc> resolveTexDesc;
	};
	enum class TextureAccess : uint8_t {
		Sampled,
		StorageRead,
		StorageWrite,
		StorageReadWrite,
		TransferRead,
		TransferWrite,
	};
	enum class BufferAccess : uint8_t {
		ShaderRead,
		ShaderWrite,
		ShaderReadWrite,
		IndexRead,
		IndirectRead,
		TransferRead,
		TransferWrite,
	};
	// not actually passed directly to dependency calls right now
	struct TextureDependencyDesc {
		TextureDesc texDesc;
		TextureAccess access = TextureAccess::Sampled;
	};
	struct BufferDependencyDesc {
		Buffer& buffer;
		BufferAccess access = BufferAccess::ShaderRead;
	};

	struct UploadData;
	struct CopyOp {
		GraphTexture src;
		GraphTexture dst;
	};
	struct BlitOp {
		GraphTexture src;
		GraphTexture dst;
	};
	struct UpdateOp {
		BufferHandle dst;
		size_t dstOffset = 0;
		std::function<UploadData()> dataCb;
	};
	struct UploadOp {
		BufferHandle dst;
		size_t dstOffset = 0;
		std::function<UploadData()> dataCb;
	};
	using IntermediateOp = std::variant<CopyOp, BlitOp, UpdateOp, UploadOp>;

	// user defined information from addPass and RenderPassBuilder
	struct PassDesc {
		enum class Type {
			Graphics,
			Compute,
			Intermediate,
			RayTracing
		};
		// these first three members are sent straight over to PassCompiled
		std::string name;
		Type type;
		std::function<void(CommandBuffer&)> executeCallback{};
		std::vector<IntermediateOp> intermediateOps;
		std::vector<AttachmentDesc> attachmentOperations;
		std::vector<TextureDependencyDesc> textureDependencyOperations;
		std::vector<BufferDependencyDesc> bufferDependencyOperations;
		uint32_t layerCount = 1;
		uint32_t viewMask = 0;
		std::function<bool()> conditionCallback{};
    	QueueAffinity queue;
		bool executeInOrder = false;
	};
} // namespace mythril
