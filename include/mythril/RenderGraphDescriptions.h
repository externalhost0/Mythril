//
// Created by Hayden Rivas on 1/20/26.
//

#pragma once

#include "Objects.h"

#include <cstddef>
#include <optional>
#include <span>
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
        VkClearColorValue getAsVkClearColorValue() const { return { { r, g, b, a } }; }
    };

    struct ClearDepthStencil {
        float depth;
        uint32_t stencil;
        ClearDepthStencil(float depth, uint32_t stencil) :
            depth(depth),
            stencil(stencil) {}
        VkClearDepthStencilValue getAsVkClearDepthStencilValue() const { return { depth, stencil }; }
    };

    union ClearValue {
        ClearColor clearColor;
        ClearDepthStencil clearDepthStencil;

        // if the user sets no clear (have no intention of clearing) then do nothing ig
        ClearValue() {} // NOLINT(cppcoreguidelines-pro-type-member-init)
        static ClearValue color(float r, float g, float b, float a) {
            ClearValue v;
            v.clearColor = { r, g, b, a };
            return v;
        }
        static ClearValue depth(float depth, uint32_t stencil = 0) {
            ClearValue v;
            v.clearDepthStencil = { depth, stencil };
            return v;
        }
        VkClearValue getDepthStencilValue() const { return {
            .depthStencil = clearDepthStencil.getAsVkClearDepthStencilValue(),
        }; }
        VkClearValue getColorValue() const { return {
            .color = clearColor.getAsVkClearColorValue(),
        }; }
    };


    class RenderGraph;
    struct GraphTextureAccess;
    struct GraphBufferAccess;
    struct GraphAccelerationStructureAccess;
    // one version of a texture as one RenderGraph knows it, a pass that writes it returns the next version
    class GraphTexture {
    public:
        constexpr GraphTexture() = default;
        constexpr bool valid() const { return _index != kInvalid; }
        constexpr bool operator==(const GraphTexture&) const = default;
    private:
        static constexpr uint32_t kInvalid = UINT32_MAX;
        explicit constexpr GraphTexture(uint32_t index, uint32_t version = 0) :
            _index(index),
            _version(version) {}
        uint32_t _index = kInvalid;
        uint32_t _version = 0;
        friend class RenderGraph;
        friend struct GraphTextureAccess;
    };
    // one version of a buffer imported into a RenderGraph, a pass that writes it returns the next version
    class GraphBuffer {
    public:
        constexpr GraphBuffer() = default;
        constexpr bool valid() const { return _index != kInvalid; }
        constexpr bool operator==(const GraphBuffer&) const = default;
    private:
        static constexpr uint32_t kInvalid = UINT32_MAX;
        explicit constexpr GraphBuffer(uint32_t index, uint32_t version = 0) :
            _index(index),
            _version(version) {}
        uint32_t _index = kInvalid;
        uint32_t _version = 0;
        friend class RenderGraph;
        friend struct GraphBufferAccess;
    };
    // one version of an acceleration structure imported into a RenderGraph, a build or refit returns the next version
    class GraphAccelerationStructure {
    public:
        constexpr GraphAccelerationStructure() = default;
        constexpr bool valid() const { return _storageSlot != kInvalid; }
        constexpr bool operator==(const GraphAccelerationStructure&) const = default;
    private:
        static constexpr uint32_t kInvalid = UINT32_MAX;
        explicit constexpr GraphAccelerationStructure(uint32_t storageSlot, uint32_t version = 0) :
            _storageSlot(storageSlot),
            _version(version) {}
        uint32_t _storageSlot = kInvalid;
        uint32_t _version = 0;
        friend class RenderGraph;
        friend struct GraphAccelerationStructureAccess;
    };

    class GraphExtent {
    public:
        static constexpr GraphExtent swapchain() { return GraphExtent{ 0 }; }
        constexpr bool operator==(const GraphExtent&) const = default;
    private:
        explicit constexpr GraphExtent(uint32_t index) :
            _index(index) {}
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

    // what CommandBuffer::texture returns, valid for the pass being recorded
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
    // the versions an attachment writes, resolve is invalid without a resolve target
    struct AttachmentOutput {
        GraphTexture texture;
        GraphTexture resolve;
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
        AccelerationStructureRead,
        AccelerationStructureBuildInput,
        AccelerationStructureBuildRead,
        AccelerationStructureBuildWrite,
        AccelerationStructureScratch,
    };
    // builds and refits are TransferBuilder ops, so a pass only declares reads
    enum class AccelerationStructureAccess : uint8_t {
        Read,
    };
    // not actually passed directly to dependency calls right now
    struct TextureDependencyDesc {
        TextureDesc texDesc;
        TextureAccess access = TextureAccess::Sampled;
    };
    struct BufferDependencyDesc {
        GraphBuffer buffer;
        BufferHandle handle;
        BufferAccess access = BufferAccess::ShaderRead;
    };

    struct CopyOp {
        GraphTexture src;
        GraphTexture dst;
    };
    struct BlitOp {
        GraphTexture src;
        GraphTexture dst;
    };
    // bytes live in the graph's per-frame arena
    struct UpdateOp {
        BufferHandle dst;
        size_t dstOffset = 0;
        std::span<const std::byte> bytes;
    };
    struct UploadOp {
        BufferHandle dst;
        size_t dstOffset = 0;
        std::span<const std::byte> bytes;
    };
    struct BufferCopyOp {
        BufferHandle src;
        BufferHandle dst;
    };
    struct ReadbackOp {
        TextureDesc src;
        BufferHandle dst;
    };
    struct GenerateMipsOp {
        GraphTexture texture;
    };
    struct ImageUploadOp {
        TextureDesc dst;
        std::span<const std::byte> bytes;
    };
    struct AccelerationStructureBuildOp {
        AccelerationStructureHandle structure;
        bool refit = false;
        // the BLAS storage versions a TLAS build reads
        std::vector<GraphBuffer> instanced;
    };
    using TransferOp = std::variant<CopyOp, BlitOp, UpdateOp, UploadOp, BufferCopyOp, ReadbackOp, GenerateMipsOp, ImageUploadOp, AccelerationStructureBuildOp>;

    // sync with work outside the graph (OpenXR, video, CUDA, another process), pass it on import every frame
    struct ExternalAccess {
        // the first batch that uses the resource waits on it
        VkSemaphore waitSemaphore = VK_NULL_HANDLE;
        uint64_t waitValue = 0;
        // the last batch that uses the resource signals it
        VkSemaphore signalSemaphore = VK_NULL_HANDLE;
        uint64_t signalValue = 0;
        // textures only, UNDEFINED on entry drops the contents and on exit keeps the graph's last layout
        VkImageLayout entryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout exitLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // the family that owns it outside the frame, the transfers keep the layout (entry on acquire, exit on release)
        uint32_t ownerFamily = VK_QUEUE_FAMILY_IGNORED;
    };

    struct PassExecute {
        void (*invoke)(const PassExecute& self, CommandBuffer& cmd) = nullptr;
    };

    // user defined information from addPass and RenderPassBuilder
    struct PassDesc {
        enum class Type {
            Graphics,
            Compute,
            Transfer,
            RayTracing
        };
        // these first three members are sent straight over to PassCompiled
        std::string name;
        Type type{};
        const PassExecute* execute = nullptr;
        std::vector<TransferOp> transferOps;
        std::vector<AttachmentDesc> attachmentOperations;
        std::vector<TextureDependencyDesc> textureDependencyOperations;
        std::vector<BufferDependencyDesc> bufferDependencyOperations;
        uint32_t layerCount = 1;
        uint32_t viewMask = 0;
        // the queue comes from the pass kind at compile, async only asks for one
        bool async = false;
        bool executeInOrder = false;
        bool sideEffect = false;
    };
} // namespace mythril
