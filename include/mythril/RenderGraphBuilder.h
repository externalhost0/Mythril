//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "Objects.h"
#include "../../lib/ObjectHandles.h"
#include "../../lib/vkenums.h"
#include "../../lib/RenderGraphDescriptions.h"
#include "../../lib/vkutil.h"


#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <functional>
#include <utility>
#include <span>

#include <volk.h>

namespace mythril {
    struct DependencyDesc;
    struct AttachmentDesc;
    class CommandBuffer;
    class RenderGraph;

    struct AttachmentInfo {
        VkFormat imageFormat; // the only field that RenderingAttachmentInfo doesnt need but GraphcsPipeline does
        VkImageView imageView;
        VkImageLayout imageLayout;
        // optional resolve target
        VkImageView resolveImageView;
        VkImageLayout resolveImageLayout;

        VkAttachmentLoadOp loadOp;
        VkAttachmentStoreOp storeOp;

        VkClearValue clearValue;

        VkRenderingAttachmentInfo getAsVkRenderingAttachmentInfo() const {
            const bool isResolving = resolveImageView != VK_NULL_HANDLE;
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = imageView,
                .imageLayout = imageLayout,
                .resolveMode = isResolving ? (vkutil::IsIntegerFormat(imageFormat) || vkutil::IsFormatDepthOrStencil(imageFormat) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT) : VK_RESOLVE_MODE_NONE,
                .resolveImageView = resolveImageView,
                .resolveImageLayout = resolveImageLayout,
                .loadOp = loadOp,
                .storeOp = storeOp,
                .clearValue = clearValue
            };
        }
    };

    struct SubresourceState {
        VkImageLayout layout; // uint32_t
        vkutil::StageAccess mask; // uint64_t

        bool operator==(const SubresourceState& o) const {
            return layout == o.layout && mask.access == o.mask.access && mask.stage == o.mask.stage;
        }
        bool operator!=(const SubresourceState& o) const {
            return !(*this == o);
        }
    };

    // i mean we could decrease memory by 2x if we want to limit the user
    struct SubresourceRange {
        uint32_t baseMip;
        uint32_t numMips;
        uint32_t baseLayer;
        uint32_t numLayers;

        // check if this range completely contains another range
        bool contains(const SubresourceRange& o) const {
            const uint32_t mipEnd = baseMip + numMips;
            const uint32_t otherMipEnd = o.baseMip + o.numMips;
            const uint32_t layerEnd = baseLayer + numLayers;
            const uint32_t otherLayerEnd = o.baseLayer + o.numLayers;

            return baseMip <= o.baseMip && mipEnd >= otherMipEnd &&
                   baseLayer <= o.baseLayer && layerEnd >= otherLayerEnd;
        }
        // check if ranges overlap in any way
        bool overlaps(const SubresourceRange& o) const {
            const uint32_t mipEnd = baseMip + numMips;
            const uint32_t otherMipEnd = o.baseMip + o.numMips;
            const uint32_t layerEnd = baseLayer + numLayers;
            const uint32_t otherLayerEnd = o.baseLayer + o.numLayers;

            const bool mipOverlap = !(mipEnd <= o.baseMip || baseMip >= otherMipEnd);
            const bool layerOverlap = !(layerEnd <= o.baseLayer || baseLayer >= otherLayerEnd);

            return mipOverlap && layerOverlap;
        }

        bool operator==(const SubresourceRange& o) const {
            return baseMip == o.baseMip && numMips == o.numMips && baseLayer == o.baseLayer && numLayers == o.numLayers;
        }
        bool operator!=(const SubresourceRange& o) const {
            return !(*this == o);
        }
    };

    class TextureStateTracker {
    public:
        TextureStateTracker(uint32_t mips, uint32_t layers)
            : totalMips(mips), totalLayers(layers) {
            wholeResourceState = SubresourceState();
            isWholeResource = true;
        }
        SubresourceState getState(const SubresourceRange& range) const {
            if (isWholeResource) {
                return wholeResourceState;
            }
            // chcek for exact or containing range
            for (auto& entry: subresourceStates) {
                if (entry.range == range || entry.range.contains(range)) {
                    return entry.state;
                }
            }
            // not found then return undefined state
            return {};
        }
        void setState(const SubresourceRange& range, const SubresourceState& state) {
            // fast path: updating entire resource
            if (const SubresourceRange wholeRange = {0, totalMips, 0, totalLayers}; range == wholeRange) {
                wholeResourceState = state;
                isWholeResource = true;
                subresourceStates.clear();
                return;
            }
            // needs subresource tracking
            if (isWholeResource) {
                isWholeResource = false;
                subresourceStates.clear();
            }
            // attempt to find exisiting entry
            for (auto& entry: subresourceStates) {
                if (entry.range == range) {
                    entry.state = state;
                    return;
                }
            }
            subresourceStates.emplace_back(range, state);
        }
        uint32_t getTotalMips() const { return totalMips; }
        uint32_t getTotalLayers() const { return totalLayers; }

    private:
        uint32_t totalMips;
        uint32_t totalLayers;

        // fast path: single state for entire resource, which is most common
        SubresourceState wholeResourceState{};
        bool isWholeResource;

        // slow path: per subresource tracking
        struct SubresourceEntry {
            SubresourceRange range;
            SubresourceState state;
        };

        std::vector<SubresourceEntry> subresourceStates;
    };

    struct CompiledImageBarrier {
        // we dont want to only rely on a vkImage as when the texture updates we need to be able to fetch its new version
        TextureHandle handle;
        // TextureHandle textureHandle;
        SubresourceRange range{};
        VkImageLayout dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkutil::StageAccess dstMask{};
        bool isSwapchain = false;
    };

    // hidden information that transforms the PassSource into usable info
    struct CompiledPass {
        // transferred info from source
        std::string name;
        uint32_t passIndex;
        PassDesc::Type type;

        std::vector<CompiledImageBarrier> imageBarriers;
        std::function<void(CommandBuffer&)> executeCallback;

        // new info transformed from source on compile()
        std::vector<AttachmentInfo> colorAttachments;
        std::optional<AttachmentInfo> depthAttachment;
        VkRect2D renderArea;
    };

    // builders are how the user interacts with a capability
    // BasePassBuilder is a blueprint for other PassBuilders only
    struct BasePassBuilder {
        BasePassBuilder() = delete;
        BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type)
            : _rGraph(rGraph), _passSource(pName, type) {
            ASSERT(!_passSource.name.empty());
        }
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

    class GraphicsPassBuilder {
    public:
        GraphicsPassBuilder() = delete;
        GraphicsPassBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Graphics) {}

        GraphicsPassBuilder& attachment(const AttachmentDesc& desc) {
            this->base._passSource.attachmentOperations.push_back(desc);
            return *this;
        }
        GraphicsPassBuilder& dependency(const TextureDesc& texDesc, const Layout layout = Layout::READ) {
            add(this->base._passSource, texDesc, layout);
            return *this;
        }
        GraphicsPassBuilder& dependency(Texture* tex, int count, const Layout layout = Layout::READ) {
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

        ComputePassBuilder& dependency(const TextureDesc& texDesc, const Layout layout = Layout::GENERAL) {
                add(this->base._passSource, texDesc, layout);
            return *this;
        }
        ComputePassBuilder& dependency(Texture* tex, int count, const Layout layout = Layout::GENERAL) {
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
        void finish();
    private:
        BasePassBuilder base;
    };

    class RenderGraph {
    public:
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

    private:
        void PerformImageBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass);
        static void processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass);
        void processPassResources(const PassDesc& passDesc, CompiledPass& outPass);
        void processAttachments(const PassDesc& pass_desc, CompiledPass& outPass);
        void performDryRun(CTX& rCtx);
        // every texture used in the framegraph needs proper tracking to ensure its layout is correct...
        // at every pass, even more necessary for textures that request individual mips/layers
        std::unordered_map<TextureHandle, TextureStateTracker> _resourceTrackers;
        // data used during compilation
        std::vector<PassDesc> _passDescriptions;
        // data fetched during execution
        std::vector<CompiledPass> _compiledPasses;

        bool _hasCompiled = false;

        friend struct BasePassBuilder;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class IntermediateBuilder;
    };
}
