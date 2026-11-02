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

    // what the graph last recorded per subresource, only trusted while the texture's layout version still matches
    struct TextureAccessCache {
        SubresourceMap<StageAccess> masks;
        uint32_t layoutVersion = 0;
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
        std::vector<IntermediateOp> intermediateOps;
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
