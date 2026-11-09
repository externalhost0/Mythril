//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once
#include "mythril/RenderGraphBuilder.h"

#include <cstdint>
#include <volk.h>

namespace mythril::vkutil {
	using StageAccess = ::mythril::StageAccess;

#ifdef DEBUG
	void SetObjectDebugName(VkDevice device, VkObjectType objectType, uint64_t handle, const char* name);
#else
	inline void SetObjectDebugName(VkDevice, VkObjectType, uint64_t, const char*) {}
#endif
	uint32_t GetTextureBytesPerPlane(uint32_t width, uint32_t height, VkFormat format, uint32_t plane);
	uint32_t GetTextureBytesPerLayer(uint32_t width, uint32_t height, VkFormat format, uint32_t level);
	uint32_t GetBytesPerPixel(VkFormat format);
	uint32_t GetNumImagePlanes(VkFormat format);
	VkExtent2D GetImagePlaneExtent(VkExtent2D plane0, VkFormat format, uint32_t plane);


	constexpr uint32_t GetAlignedSize(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
	constexpr VkImageAspectFlags AspectMaskFromAttachmentLayoutEXT(VkImageLayout layout) {
		switch (layout) {
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
				return VK_IMAGE_ASPECT_STENCIL_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
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
			default:
				return false;
		}
	}
	constexpr bool IsFormatStencil(VkFormat format) {
		switch (format) {
			case VK_FORMAT_S8_UINT:

			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return true;
			default:
				return false;
		}
	}
	constexpr bool IsFormatDepthOrStencil(VkFormat format) { return IsFormatDepth(format) || IsFormatStencil(format); }
	constexpr bool IsFormatDepthAndStencil(VkFormat format) { return IsFormatDepth(format) && IsFormatStencil(format); }

	StageAccess GetPipelineStageAccess(VkImageLayout layout);
	void ImageMemoryBarrier2(VkCommandBuffer cmd, VkImage image, StageAccess src, StageAccess dst, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange range);

	constexpr VkBufferUsageFlags ToVkBufferUsage(uint8_t usageBits, StorageType storage) {
		VkBufferUsageFlags usage = (storage == StorageType::Device) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0;
		if (usageBits & BufferUsageBits_Index)
			usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (usageBits & BufferUsageBits_Storage)
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (usageBits & BufferUsageBits_Indirect)
			usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (usageBits & BufferUsageBits_AccelerationStructureInputReadOnly)
			usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (usageBits & BufferUsageBits_AccelerationStructureStorage)
			usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (usageBits & BufferUsageBits_ShaderBindingTable)
			usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		return usage;
	}

	// ray tracing pipeline requires acceleration structure, so {false, true} is never valid
	struct BufferSyncFeatures {
		bool accelerationStructure;
		bool rayTracingPipeline;
	};
	enum class BufferUseGate : uint8_t {
		None,
		AccelerationStructure,
		RayTracingPipeline,
	};
	struct BufferUse {
		VkBufferUsageFlags usage;
		VkPipelineStageFlags2 stages;
		VkAccessFlags2 access;
		BufferUseGate gate;
	};
	constexpr VkPipelineStageFlags2 kAnyShaderStage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
	constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
	                                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_MEMORY_WRITE_BIT;
	// every way a buffer made by CTX::createBuffer can be accessed on the device
	constexpr BufferUse kBufferUses[] = {
	    {VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, kAnyShaderStage, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, BufferUseGate::None},
	    {VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, BufferUseGate::AccelerationStructure},
	    {VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, BufferUseGate::None},
	    {VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, BufferUseGate::None},
	    {VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, BufferUseGate::AccelerationStructure},
	    {VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT, BufferUseGate::AccelerationStructure},
	    {VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, BufferUseGate::AccelerationStructure},
	    {VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, kAnyShaderStage, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, BufferUseGate::AccelerationStructure},
	    {VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR, BufferUseGate::RayTracingPipeline},
	    {VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, BufferUseGate::None},
	    {VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, BufferUseGate::None},
	};
	constexpr VkBufferUsageFlags kHandledBufferUsage = [] {
		VkBufferUsageFlags usage = 0;
		for (const BufferUse& use : kBufferUses)
			usage |= use.usage;
		return usage;
	}();

	struct BufferBarrier {
		StageAccess src;
		StageAccess dst;
	};
	struct UpdateBufferBarriers {
		BufferBarrier before;
		BufferBarrier after;
	};
	constexpr UpdateBufferBarriers GetUpdateBufferBarriers(VkBufferUsageFlags usage, BufferSyncFeatures features) {
		const auto gateOpen = [&](BufferUseGate gate) {
			switch (gate) {
				case BufferUseGate::None: return true;
				case BufferUseGate::AccelerationStructure: return features.accelerationStructure;
				case BufferUseGate::RayTracingPipeline: return features.rayTracingPipeline;
			}
			return false;
		};
		const VkPipelineStageFlags2 enabledStages = features.rayTracingPipeline ? ~VkPipelineStageFlags2{0} : ~VkPipelineStageFlags2{VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR};

		StageAccess consumers = {};
		for (const BufferUse& use : kBufferUses) {
			if (!(use.usage & usage) || !gateOpen(use.gate))
				continue;
			consumers.stage |= use.stages & enabledStages;
			consumers.access |= use.access;
		}
		const StageAccess update = {.stage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT};
		return {
		    .before = {.src = {.stage = consumers.stage, .access = consumers.access & kWriteAccess}, .dst = update},
		    .after = {.src = update, .dst = consumers},
		};
	}

	// uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

	VkSemaphore CreateTimelineSemaphore(VkDevice device, unsigned int numImages);

	constexpr uint32_t CalcNumMipLevels(uint32_t width, uint32_t height) {
		uint32_t levels = 1;
		while ((width | height) >> levels)
			levels++;
		return levels;
	}
	constexpr bool IsComponentMappingAnIdentity(VkComponentMapping componentMapping) {
		return componentMapping.r == VK_COMPONENT_SWIZZLE_IDENTITY && componentMapping.g == VK_COMPONENT_SWIZZLE_IDENTITY && componentMapping.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
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
			default:
				return false;
		}
	}
} // namespace mythril::vkutil
