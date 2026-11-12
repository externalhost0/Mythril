//
// Created by Hayden Rivas on 10/8/25.
//
// Internal compiled-stage types for the render graph. Defined here so the
// public include/mythril/RenderGraphBuilder.h can forward-declare them and
// stay free of lib/ reaches.
//

#pragma once

#include "mythril/RenderGraphBuilder.h"
#include "mythril/RenderGraphDescriptions.h"
#include "mythril/SwapchainSpec.h"
#include "SubresourceMap.h"
#include "vkutil.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include <volk.h>


namespace mythril {

    struct GraphTextureAccess {
        static constexpr uint32_t index(GraphTexture texture) { return texture._index; }
    };
    struct GraphTextureHash {
        size_t operator()(const GraphTexture texture) const { return std::hash<uint32_t>{}(GraphTextureAccess::index(texture)); }
    };

    struct TextureUses {
        bool attachment = false;
        bool sampled = false;
        bool storage = false;
        bool transfer = false;

        void addAttachment() { attachment = true; }
        void add(const TextureAccess access) {
            switch (access) {
                case TextureAccess::Sampled:
                    sampled = true;
                    break;
                case TextureAccess::StorageRead:
                case TextureAccess::StorageWrite:
                case TextureAccess::StorageReadWrite:
                    storage = true;
                    break;
                case TextureAccess::TransferRead:
                case TextureAccess::TransferWrite:
                    transfer = true;
                    break;
            }
        }
    };
    struct Realization {
        Dimensions dimensions;
        uint8_t usage = 0;
        StorageType storage = StorageType::Device;
        bool operator==(const Realization&) const = default;
    };
    inline Dimensions ScaleExtent(const Dimensions extent, const float scale) {
        const auto scaled = [scale](const uint32_t v) { return std::max(1u, static_cast<uint32_t>(static_cast<float>(v) * scale)); };
        return {scaled(extent.width), scaled(extent.height), 1};
    }
    inline Realization InferRealization(const Dimensions dimensions, const TextureUses& uses, const bool exported, const bool confined) {
        uint8_t usage = 0;
        if (uses.attachment)
            usage |= TextureUsageBits_Attachment;
        if (uses.sampled || exported)
            usage |= TextureUsageBits_Sampled;
        if (uses.storage)
            usage |= TextureUsageBits_Storage;
        const bool memoryless = usage == TextureUsageBits_Attachment && !uses.transfer && confined;
        return {.dimensions = dimensions, .usage = usage, .storage = memoryless ? StorageType::Memoryless : StorageType::Device};
    }
    inline TextureSpec ToTextureSpec(const GraphTextureSpec& spec, const Realization& realization) {
        return {
            .dimension = realization.dimensions,
            .type = spec.type,
            .format = spec.format,
            .samples = spec.samples,
            .usage = realization.usage,
            .storage = realization.storage,
            .numMipLevels = spec.numMipLevels,
            .numLayers = spec.numLayers,
            .components = spec.components,
            .debugName = spec.debugName.c_str(),
        };
    }

    struct DeclaredTexture {
        GraphTextureSpec spec;
        Texture texture;
        Realization realized;
        bool exported = false;
    };
    struct ImportedTexture {
        TextureHandle handle;
    };
    struct SwapchainTexture {};
    struct GraphResource {
        std::variant<DeclaredTexture, ImportedTexture, SwapchainTexture> source;
    };

    struct TextureShape {
        std::string_view debugName;
        VkFormat format;
        uint8_t sampleCount;
        Dimensions dimensions;
        uint32_t numMips;
        uint32_t numLayers;
    };

    struct AttachmentTarget {
        GraphTexture resource;
        SubresourceRange range;
        bool subresourceView = false;
    };

    struct AttachmentInfo {
        VkFormat imageFormat;
        VkImageView imageView;
        VkImageLayout imageLayout;
        VkImageView resolveImageView;
        VkImageLayout resolveImageLayout;

        VkAttachmentLoadOp loadOp;
        VkAttachmentStoreOp storeOp;

        VkClearValue clearValue;
        AttachmentTarget target;
        std::optional<AttachmentTarget> resolveTarget;
        // imageView, resolveImageView, texture and swapchainImageViews are filled by bind after merging
        TextureHandle texture;

        // will need to handle the special case of using a swapchain image as an attachment
        bool isSwapchainImage = false;
        VkImageView swapchainImageViews[kMAX_SWAPCHAIN_IMAGES] = {};

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

    // what the graph last recorded per subresource, only trusted while the texture's layout version still matches
    struct TextureAccessCache {
        SubresourceMap<StageAccess> masks;
        uint32_t layoutVersion = 0;
    };

    struct CompiledImageBarrier {
        GraphTexture resource;
        // filled by bind, unused for the swapchain which resolves per frame
        TextureHandle handle;
        SubresourceRange range{};
        VkImageLayout dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        StageAccess dstMask{};
        bool isSwapchain = false;
    };
    struct CompiledBufferBarrier {
        BufferHandle handle;
        StageAccess dstMask{};
    };

    struct UploadData {
        const void* data = nullptr;
        size_t size = 0;
    };

    struct PassMember {
        std::string name;
        std::function<void(CommandBuffer&)> callback;
        std::vector<GraphTexture> sortedDeclaredTextures;
    };

    // hidden information that transforms the PassSource into usable info
    struct CompiledPass {
        // transferred info from source
        std::string name;
        PassDesc::Type type;

        std::vector<CompiledImageBarrier> imageBarriers;
        std::vector<CompiledBufferBarrier> bufferBarriers;
        std::vector<CompiledImageBarrier> exitBarriers;
        // callbacks recorded in order inside this pass's barriers and rendering scope, empty for intermediate passes
        std::vector<PassMember> members;
        std::vector<IntermediateOp> intermediateOps;
        // the passes right before this one were moved ahead of it so it could merge
        uint32_t hoistedAheadCount = 0;
        std::function<bool()> conditionCallback;
        std::optional<VkMemoryBarrier2> inOrderBarrier;

        // new info transformed from source on compile()
        std::vector<AttachmentInfo> colorAttachments;
        std::optional<AttachmentInfo> depthAttachment;
        VkRect2D renderArea;
        uint32_t layerCount = 1;
        uint32_t viewMask = 0;
    	QueueAffinity queue = QueueAffinity::Graphics;
    };

    // indexed by GraphTexture id
    inline std::vector<bool> ConfinedToOneScope(const std::span<const CompiledPass> groups, const size_t textureCount) {
        constexpr size_t kNoGroup = SIZE_MAX;
        std::vector<uint32_t> groupsUsing(textureCount, 0);
        std::vector<size_t> lastGroup(textureCount, kNoGroup);
        std::vector<bool> isAttachment(textureCount, false);
        std::vector<bool> disqualified(textureCount, false);
        for (size_t groupIdx = 0; groupIdx < groups.size(); ++groupIdx) {
            const CompiledPass& group = groups[groupIdx];
            for (const PassMember& member: group.members) {
                for (const GraphTexture texture: member.sortedDeclaredTextures) {
                    const uint32_t id = GraphTextureAccess::index(texture);
                    if (lastGroup[id] != groupIdx) {
                        lastGroup[id] = groupIdx;
                        ++groupsUsing[id];
                    }
                }
            }
            const auto visit = [&](const AttachmentInfo& attachment) {
                const uint32_t id = GraphTextureAccess::index(attachment.target.resource);
                isAttachment[id] = true;
                if (attachment.isSwapchainImage || attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || attachment.storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
                    disqualified[id] = true;
                if (attachment.resolveTarget.has_value())
                    disqualified[GraphTextureAccess::index(attachment.resolveTarget->resource)] = true;
            };
            for (const AttachmentInfo& color: group.colorAttachments)
                visit(color);
            if (group.depthAttachment.has_value())
                visit(*group.depthAttachment);
        }
        std::vector<bool> confined(textureCount, false);
        for (size_t id = 0; id < textureCount; ++id)
            confined[id] = groupsUsing[id] == 1 && isAttachment[id] && !disqualified[id];
        return confined;
    }

    std::vector<uint32_t> cull_unused_passes(const std::vector<uint32_t>& topo_sorted_order, const std::vector<std::unordered_set<uint32_t>>& predecessors, const std::vector<PassDesc>& pass_descs, const std::vector<GraphResource>& textures);

    // only pass types resolve here so LOG_PASS and ASSERT_PASS can't be handed a loose string
    inline std::string_view PassLogName(const PassDesc& pass) { return pass.name; }
    inline std::string_view PassLogName(const CompiledPass& pass) { return pass.name; }

} // namespace mythril
