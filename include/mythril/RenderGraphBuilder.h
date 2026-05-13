//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "Objects.h"
#include "ObjectHandles.h"
#include "RenderGraphDescriptions.h"

#include <volk.h>

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace mythril {
    struct DependencyDesc;
    struct AttachmentDesc;
    class CommandBuffer;
    class CTX;
    class RenderGraph;

    // GPU pipeline stage/access pair tracked per buffer in the render graph.
    struct StageAccess {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 access;
    };

    // forward declarations for compiled/internal types (defined in lib/RenderGraphInternal.h)
    struct AttachmentInfo;
    struct SubresourceRange;
    struct SubresourceState;
    class TextureStateTracker;
    struct CompiledImageBarrier;
    struct CompiledBufferBarrier;
    struct UploadData;
    struct CompiledPass;

    // builders are how the user interacts with a capability
    // BasePassBuilder is a blueprint for other PassBuilders only
    struct BasePassBuilder {
        BasePassBuilder() = delete;
        BasePassBuilder(RenderGraph& rGraph, const char* pName, PassDesc::Type type);
    protected:
        RenderGraph& _rGraph;
        void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback);
    private:
        PassDesc _passSource;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class IntermediateBuilder;
    };


    inline void add(PassDesc& passSource, const TextureDesc& desc, const Layout layout) {
        passSource.dependencyOperations.emplace_back(desc, layout);
    }
    inline void add(PassDesc& passSource, Buffer& buffer, const BufferAccess access) {
        passSource.bufferDependencyOperations.push_back({buffer, access});
    }

    class GraphicsPassBuilder {
    public:
        GraphicsPassBuilder() = delete;
        GraphicsPassBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Graphics) {}
#ifdef DEBUG
        ~GraphicsPassBuilder();
#endif

        [[nodiscard]] GraphicsPassBuilder& attachment(const AttachmentDesc& desc) {
            this->base._passSource.attachmentOperations.push_back(desc);
            return *this;
        }
        [[nodiscard]] GraphicsPassBuilder& dependency(const TextureDesc& texDesc, const Layout layout = Layout::READ) {
            add(this->base._passSource, texDesc, layout);
            return *this;
        }
        [[nodiscard]] GraphicsPassBuilder& dependency(Buffer& buffer, const BufferAccess access) {
            add(this->base._passSource, buffer, access);
            return *this;
        }
        [[nodiscard]] GraphicsPassBuilder& dependency(Texture* tex, int count, const Layout layout = Layout::READ) {
            for (int i = 0; i < count; i++) {
                add(this->base._passSource, tex[i], layout);
            }
            return *this;
        }

        void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback) {
            base.setExecuteCallback(callback);
        }
    private:
        BasePassBuilder base;
    };

    class ComputePassBuilder {
    public:
        ComputePassBuilder() = delete;
        ComputePassBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Compute) {}
#ifdef DEBUG
        ~ComputePassBuilder();
#endif

        [[nodiscard]] ComputePassBuilder& dependency(const TextureDesc& texDesc, const Layout layout = Layout::GENERAL) {
                add(this->base._passSource, texDesc, layout);
            return *this;
        }
        [[nodiscard]] ComputePassBuilder& dependency(Buffer& buffer, const BufferAccess access) {
            add(this->base._passSource, buffer, access);
            return *this;
        }
        [[nodiscard]] ComputePassBuilder& dependency(Texture* tex, int count, const Layout layout = Layout::GENERAL) {
            for (int i = 0; i < count; i++) {
                add(this->base._passSource, tex[i], layout);
            }
            return *this;
        }

        void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback) {
            base.setExecuteCallback(callback);
        }
    private:
        BasePassBuilder base;
    };

    class IntermediateBuilder {
    public:
        IntermediateBuilder() = delete;
        IntermediateBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Intermediate) {}

        // helper that encomposses a order of the below fuctions
        IntermediateBuilder& copy(TextureDesc src, TextureDesc dst);
        IntermediateBuilder& blit(TextureDesc src, TextureDesc dst);
        IntermediateBuilder& generateMipmaps(const Texture& texture);
        IntermediateBuilder& dependency(Buffer& buffer, BufferAccess access);
        IntermediateBuilder& update(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset = 0);
        IntermediateBuilder& upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset = 0);
        void finish();
    private:
        BasePassBuilder base;
    };

    class RenderGraph {
    public:
        RenderGraph();
        ~RenderGraph();
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) noexcept;
        RenderGraph& operator=(RenderGraph&&) noexcept;

        // setup steps are designed with minimal overhead
        // hence why we simply take directly the data the user gives and stores it, nothing else
        [[nodiscard]] GraphicsPassBuilder addGraphicsPass(const char* pName) {
            return GraphicsPassBuilder{*this, pName};
        }
        [[nodiscard]] ComputePassBuilder addComputePass(const char* pName) {
            return ComputePassBuilder{*this, pName};
        }
        [[nodiscard]] IntermediateBuilder addIntermediate(const char* pName) {
            return IntermediateBuilder{*this, pName};
        }
        // compile is where ALL of the overhead should be, all data stored during setup is now translated into pieces that are easier to direct with
        // called sparingly, perhaps once
        void compile(CTX& rCtx);
        // called every frame
        void execute(CommandBuffer& cmd);
        void trackWindowSized(Texture& texture);
        void trackWindowSized(Texture& texture, std::function<Dimensions(Dimensions)> scaleFn);
        void resizeTrackedWindowSized(const Dimensions& swapchainDimensions) const;

    private:
        void PerformBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass);
        static void processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass, std::string_view passName);
        static void processPassResources(const PassDesc& passDesc, CompiledPass& outPass);
        void processAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass);
        void performDryRun(CTX& rCtx);
        // every texture used in the framegraph needs proper tracking to ensure its layout is correct...
        // at every pass, even more necessary for textures that request individual mips/layers
        std::unordered_map<TextureHandle, TextureStateTracker> _resourceTrackers;
        std::unordered_map<BufferHandle, StageAccess> _bufferTrackers;
        struct WindowSizedTexture {
            Texture* texture = nullptr;
            std::function<Dimensions(Dimensions)> scaleFn;
        };
        std::vector<WindowSizedTexture> _windowSizedTextures;
        // data used during compilation
        std::vector<PassDesc> _passDescriptions;
        // data fetched during execution
        std::vector<CompiledPass> _compiledPasses;

        bool _hasCompiled = false;
        // epoch snapshot from CTX at last compile(). execute() auto-recompiles on mismatch.
        uint64_t _compiledEpoch = 0;

        friend struct BasePassBuilder;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class IntermediateBuilder;
    };
}
