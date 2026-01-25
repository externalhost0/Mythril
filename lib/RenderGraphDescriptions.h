//
// Created by Hayden Rivas on 1/20/26.
//

#pragma once

#include <functional>
#include <volk.h>

namespace mythril {
    struct BasePassBuilder;
    class CommandBuffer;

    struct ClearColor {
        float r, g, b, a;
        ClearColor(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {}
        VkClearColorValue getAsVkClearColorValue() const {
            return {{r, g, b, a}};
        }
    };

    struct ClearDepthStencil {
        float depth;
        uint32_t stencil;
        ClearDepthStencil(float depth, uint32_t stencil) : depth(depth), stencil(stencil) {}
        VkClearDepthStencilValue getAsVkClearDepthStencilValue() const {
            return {depth, stencil};
        }
    };

    union ClearValue {
        ClearColor clearColor;
        ClearDepthStencil clearDepthStencil;

        // if the user sets no clear (have no intention of clearing) then do nothing ig
        ClearValue() {}
        ClearValue(float r, float g, float b, float a) : clearColor{r, g, b, a} {}
        ClearValue(float depth, uint32_t stencil) : clearDepthStencil{depth, stencil} {}
        VkClearValue getDepthStencilValue() const {
            return { .depthStencil = clearDepthStencil.getAsVkClearDepthStencilValue() };
        }
        VkClearValue getColorValue() const {
            return { .color = clearColor.getAsVkClearColorValue() };
        }
    };


    struct TextureDesc {
        TextureDesc() = delete;
        TextureDesc(const Texture& tex) : texture(tex) {}
        const Texture& texture;
        std::optional<uint32_t> baseLevel;
        std::optional<uint32_t> numLevels;
        std::optional<uint32_t> baseLayer;
        std::optional<uint32_t> numLayers;
        std::optional<TextureType> type;
    };
    struct AttachmentDesc {
        TextureDesc texDesc;
        ClearValue clearValue;
        LoadOperation loadOp = LoadOperation::NO_CARE;
        StoreOperation storeOp = StoreOperation::NO_CARE;
        std::optional<TextureDesc> resolveTexDesc;
    };
    enum class Layout : uint8_t {
        GENERAL,
        READ,
        TRANSFER_SRC,
        TRANSFER_DST,
        PRESENT
    };
    // not actually passed directly to dependency calls right now
    struct DependencyDesc
    {
        TextureDesc texDesc;
        Layout desiredLayout = Layout::GENERAL;
    };

    // user defined information from addPass and RenderPassBuilder
    struct PassDesc {
        enum class Type { Graphics, Compute, Intermediate, Presentation };
        // these first three members are sent straight over to PassCompiled
        std::string name;
        Type type;
        std::function<void(CommandBuffer&)> executeCallback{};
        std::vector<AttachmentDesc> attachmentOperations;
        std::vector<DependencyDesc> dependencyOperations;
    };
}
