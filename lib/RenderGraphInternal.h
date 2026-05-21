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
#include "vkutil.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <volk.h>


namespace mythril {

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

    struct SubresourceState {
        VkImageLayout layout; // uint32_t
        StageAccess mask;     // uint64_t pair

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
            subresourceStates.push_back({wholeResourceRange(), {}});
        }
        struct SubresourceEntry {
            SubresourceRange range;
            SubresourceState state;
        };
        SubresourceState getState(const SubresourceRange& range) const {
            // check for exact or containing range
            for (auto& entry: subresourceStates) {
                if (entry.range == range || entry.range.contains(range)) {
                    return entry.state;
                }
            }
            // not found then return undefined state
            return {};
        }
        std::vector<SubresourceEntry> getOverlappingStates(const SubresourceRange& range) const {
            std::vector<SubresourceEntry> overlappingStates;
            for (const auto& entry : subresourceStates) {
                if (!entry.range.overlaps(range)) continue;
                overlappingStates.push_back({intersect(entry.range, range), entry.state});
            }
            return overlappingStates;
        }
        void setState(SubresourceRange range, const SubresourceState& state) {
            if (range.numMips == 0 || range.numLayers == 0) {
                range = wholeResourceRange();
            }
            if (range == wholeResourceRange()) {
                subresourceStates.clear();
                subresourceStates.push_back({range, state});
                return;
            }

            std::vector<SubresourceEntry> nextStates;
            nextStates.reserve(subresourceStates.size() + 4);
            for (const auto& entry : subresourceStates) {
                if (!entry.range.overlaps(range)) {
                    nextStates.push_back(entry);
                    continue;
                }
                appendDifference(nextStates, entry, range);
            }
            nextStates.push_back({range, state});
            subresourceStates = std::move(nextStates);
        }
        uint32_t getTotalMips() const { return totalMips; }
        uint32_t getTotalLayers() const { return totalLayers; }
        SubresourceRange wholeResourceRange() const { return {0, totalMips, 0, totalLayers}; }

    private:
        uint32_t totalMips;
        uint32_t totalLayers;

        std::vector<SubresourceEntry> subresourceStates;

        static SubresourceRange intersect(const SubresourceRange& a, const SubresourceRange& b) {
            const uint32_t mipBegin = std::max(a.baseMip, b.baseMip);
            const uint32_t mipEnd = std::min(a.baseMip + a.numMips, b.baseMip + b.numMips);
            const uint32_t layerBegin = std::max(a.baseLayer, b.baseLayer);
            const uint32_t layerEnd = std::min(a.baseLayer + a.numLayers, b.baseLayer + b.numLayers);
            return {mipBegin, mipEnd - mipBegin, layerBegin, layerEnd - layerBegin};
        }
        static void appendIfValid(std::vector<SubresourceEntry>& entries, const SubresourceRange& range, const SubresourceState& state) {
            if (range.numMips > 0 && range.numLayers > 0) {
                entries.push_back({range, state});
            }
        }
        static void appendDifference(std::vector<SubresourceEntry>& entries, const SubresourceEntry& entry, const SubresourceRange& removedRange) {
            const SubresourceRange overlap = intersect(entry.range, removedRange);
            const uint32_t entryMipEnd = entry.range.baseMip + entry.range.numMips;
            const uint32_t overlapMipEnd = overlap.baseMip + overlap.numMips;
            const uint32_t entryLayerEnd = entry.range.baseLayer + entry.range.numLayers;
            const uint32_t overlapLayerEnd = overlap.baseLayer + overlap.numLayers;

            appendIfValid(entries, {entry.range.baseMip, overlap.baseMip - entry.range.baseMip, entry.range.baseLayer, entry.range.numLayers}, entry.state);
            appendIfValid(entries, {overlapMipEnd, entryMipEnd - overlapMipEnd, entry.range.baseLayer, entry.range.numLayers}, entry.state);
            appendIfValid(entries, {overlap.baseMip, overlap.numMips, entry.range.baseLayer, overlap.baseLayer - entry.range.baseLayer}, entry.state);
            appendIfValid(entries, {overlap.baseMip, overlap.numMips, overlapLayerEnd, entryLayerEnd - overlapLayerEnd}, entry.state);
        }
    };

    struct CompiledImageBarrier {
        // we dont want to only rely on a vkImage as when the texture updates we need to be able to fetch its new version
        TextureHandle handle;
        // TextureHandle textureHandle;
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

    // hidden information that transforms the PassSource into usable info
    struct CompiledPass {
        // transferred info from source
        std::string name;
        uint32_t passIndex;
        PassDesc::Type type;

        std::vector<CompiledImageBarrier> imageBarriers;
        std::vector<CompiledBufferBarrier> bufferBarriers;
        std::function<void(CommandBuffer&)> executeCallback;
        std::function<bool()> conditionCallback;

        // new info transformed from source on compile()
        std::vector<AttachmentInfo> colorAttachments;
        std::optional<AttachmentInfo> depthAttachment;
        VkRect2D renderArea;
        uint32_t layerCount = 1;
        uint32_t viewMask = 0;
    	QueueAffinity queue = QueueAffinity::Graphics;
    };

} // namespace mythril
