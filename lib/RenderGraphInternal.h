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
#include <string>
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

    struct ImportedTexture {
        TextureHandle handle;
    };
    struct SwapchainTexture {};
    struct GraphResource {
        std::variant<ImportedTexture, SwapchainTexture> source;
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

    // only pass types resolve here so LOG_PASS and ASSERT_PASS can't be handed a loose string
    inline std::string_view PassLogName(const PassDesc& pass) { return pass.name; }
    inline std::string_view PassLogName(const CompiledPass& pass) { return pass.name; }

} // namespace mythril
