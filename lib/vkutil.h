//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once
#include <cstdint>
#include <volk.h>

namespace mythril::vkutil {
    struct StageAccess {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 access;
    };

    uint32_t GetTextureBytesPerPlane(uint32_t width, uint32_t height, VkFormat format, uint32_t plane);
    uint32_t GetTextureBytesPerLayer(uint32_t width, uint32_t height, VkFormat format, uint32_t level);
    uint32_t GetBytesPerPixel(VkFormat format);
    uint32_t GetNumImagePlanes(VkFormat format);
    VkExtent2D GetImagePlaneExtent(VkExtent2D plane0, VkFormat format, uint32_t plane);


    constexpr uint32_t GetAlignedSize(uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    constexpr VkImageAspectFlags AspectMaskFromAttachmentLayoutEXT(VkImageLayout layout) {
        switch (layout) {
            case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_STENCIL_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

            default: return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }
    constexpr VkImageAspectFlags AspectMaskFromFormat(VkFormat format) {
        switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    constexpr bool IsFormatDepth(VkFormat format) {
        switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D32_SFLOAT:

            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
                return true;
            default: return false;
        }
    }
    constexpr bool IsFormatStencil(VkFormat format) {
        switch (format) {
            case VK_FORMAT_S8_UINT:

            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return true;
            default: return false;
        }
    }
    constexpr bool IsFormatDepthOrStencil(VkFormat format) {
        return IsFormatDepth(format) || IsFormatStencil(format);
    }
    constexpr bool IsFormatDepthAndStencil(VkFormat format) {
        return IsFormatDepth(format) && IsFormatStencil(format);
    }

    StageAccess GetPipelineStageAccess(VkImageLayout layout);
    void ImageMemoryBarrier2(VkCommandBuffer cmd, VkImage image, StageAccess src, StageAccess dst, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange range);

    // uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

    VkSemaphore CreateTimelineSemaphore(VkDevice device, unsigned int numImages);

    constexpr uint32_t CalcNumMipLevels(uint32_t width, uint32_t height) {
        uint32_t levels = 1;
        while ((width | height) >> levels)
            levels++;
        return levels;
    }
    constexpr bool IsComponentMappingAnIdentity(VkComponentMapping componentMapping) {
        return componentMapping.r == VK_COMPONENT_SWIZZLE_IDENTITY &&
               componentMapping.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
               componentMapping.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
               componentMapping.a == VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    constexpr bool IsIntegerFormat(VkFormat format) {
        switch (format) {
            // 8-bit
            case VK_FORMAT_R8_SINT:
            case VK_FORMAT_R8_UINT:
            case VK_FORMAT_R8G8_SINT:
            case VK_FORMAT_R8G8_UINT:
            case VK_FORMAT_R8G8B8_SINT:
            case VK_FORMAT_R8G8B8_UINT:
            case VK_FORMAT_R8G8B8A8_SINT:
            case VK_FORMAT_R8G8B8A8_UINT:

            // 16-bit
            case VK_FORMAT_R16_SINT:
            case VK_FORMAT_R16_UINT:
            case VK_FORMAT_R16G16_SINT:
            case VK_FORMAT_R16G16_UINT:
            case VK_FORMAT_R16G16B16_SINT:
            case VK_FORMAT_R16G16B16_UINT:
            case VK_FORMAT_R16G16B16A16_SINT:
            case VK_FORMAT_R16G16B16A16_UINT:

            // 32-bit
            case VK_FORMAT_R32_SINT:
            case VK_FORMAT_R32_UINT:
            case VK_FORMAT_R32G32_SINT:
            case VK_FORMAT_R32G32_UINT:
            case VK_FORMAT_R32G32B32_SINT:
            case VK_FORMAT_R32G32B32_UINT:
            case VK_FORMAT_R32G32B32A32_SINT:
            case VK_FORMAT_R32G32B32A32_UINT:

            // 64-bit
            case VK_FORMAT_R64_SINT:
            case VK_FORMAT_R64_UINT:
            case VK_FORMAT_R64G64_SINT:
            case VK_FORMAT_R64G64_UINT:
            case VK_FORMAT_R64G64B64_SINT:
            case VK_FORMAT_R64G64B64_UINT:
            case VK_FORMAT_R64G64B64A64_SINT:
            case VK_FORMAT_R64G64B64A64_UINT:
                return true;
            default: return false;
        }
    }
}
