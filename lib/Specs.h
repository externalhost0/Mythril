//
// Created by Hayden Rivas on 1/11/26.
//

#pragma once

#include "vkenums.h"

#include <filesystem>

#include <volk.h>

namespace mythril {
	// https://github.com/corporateshark/lightweightvk/blob/f5598737c2179e329e519e1fe094ade1cafbc97c/lvk/LVK.h#L315
	struct Dimensions {
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;

		constexpr Dimensions divide1D(const uint32_t v) const {
			return {.width = width / v, .height = height, .depth = depth};
		}
		constexpr Dimensions divide2D(const uint32_t v) const {
			return {.width = width / v, .height = height / v, .depth = depth};
		}
		constexpr Dimensions divide3D(const uint32_t v) const {
			return {.width = width / v, .height = height / v, .depth = depth / v};
		}
		constexpr bool operator==(const Dimensions& other) const {
			return width == other.width && height == other.height && depth == other.depth;
		}
		constexpr bool operator!=(const Dimensions& other) const {
			return !(*this==other);
		}
	};

	// bits need to be uint8_t underlying type
	enum BufferUsageBits : uint8_t {
		BufferUsageBits_Index = 1 << 0,
		BufferUsageBits_Uniform = 1 << 1,
		BufferUsageBits_Storage = 1 << 2,
		BufferUsageBits_Indirect = 1 << 3
	};
	enum TextureUsageBits : uint8_t {
		TextureUsageBits_Sampled = 1 << 0,
		TextureUsageBits_Storage = 1 << 1,
		TextureUsageBits_Attachment = 1 << 2,
	};

	// strict enums can be whatever
	enum class StorageType : uint8_t {
		Device,
		HostVisible,
		Memoryless
	};

	enum class TextureType : uint8_t {
		Type_2D,
		Type_3D,
		Type_Cube
	};
	enum Swizzle : uint8_t {
		Swizzle_Default = 0,
		Swizzle_0,
		Swizzle_1,
		Swizzle_R,
		Swizzle_G,
		Swizzle_B,
		Swizzle_A,
	};

	struct TexRange
	{
		VkOffset3D offset = {};
		VkExtent3D dimensions = {1, 1, 1};

		uint32_t layer = 0;
		uint32_t numLayers = 1;
		uint32_t mipLevel = 0;
		uint32_t numMipLevels = 1;
	};
	struct ComponentMapping
	{
		Swizzle r = Swizzle_Default;
		Swizzle g = Swizzle_Default;
		Swizzle b = Swizzle_Default;
		Swizzle a = Swizzle_Default;
		bool identity() const {
			return r == Swizzle_Default && g == Swizzle_Default && b == Swizzle_Default && a == Swizzle_Default;
		}
		VkComponentMapping toVkComponentMapping() const {
			return {
				.r = static_cast<VkComponentSwizzle>(r),
				.g = static_cast<VkComponentSwizzle>(g),
				.b = static_cast<VkComponentSwizzle>(b),
				.a = static_cast<VkComponentSwizzle>(a)
			};
		}
	};
	static_assert(mythril::Swizzle::Swizzle_Default == (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY);
	static_assert(mythril::Swizzle::Swizzle_0 == (uint32_t)VK_COMPONENT_SWIZZLE_ZERO);
	static_assert(mythril::Swizzle::Swizzle_1 == (uint32_t)VK_COMPONENT_SWIZZLE_ONE);
	static_assert(mythril::Swizzle::Swizzle_R == (uint32_t)VK_COMPONENT_SWIZZLE_R);
	static_assert(mythril::Swizzle::Swizzle_G == (uint32_t)VK_COMPONENT_SWIZZLE_G);
	static_assert(mythril::Swizzle::Swizzle_B == (uint32_t)VK_COMPONENT_SWIZZLE_B);
	static_assert(mythril::Swizzle::Swizzle_A == (uint32_t)VK_COMPONENT_SWIZZLE_A);



	// Specs should be low level but still a thin wrapper around the info creation processes need
	// User arguements will be even more abstract as they will not need to implement it via code
	struct SamplerSpec {
		SamplerFilter magFilter = SamplerFilter::Linear;
		SamplerFilter minFilter = SamplerFilter::Linear;
		SamplerMipMap mipMap = SamplerMipMap::Disabled;
		SamplerWrap wrapU = SamplerWrap::Repeat;
		SamplerWrap wrapV = SamplerWrap::Repeat;
		SamplerWrap wrapW = SamplerWrap::Repeat;

		bool depthCompareEnabled = false;
		CompareOp depthCompareOp = CompareOp::LessEqual;

		uint8_t mipLodMin = 0;
		uint8_t mipLodMax = 15;

		bool anistrophic = false;
		uint8_t maxAnisotropic = 1;
		const char* debugName = "Unnamed Sampler";
	};
	struct BufferSpec {
		size_t size = 0;
		uint8_t usage = {};
		StorageType storage = StorageType::HostVisible;
		const void* initialData = nullptr;
		const char* debugName = "Unnamed Buffer";
	};
	struct TextureSpec {
		Dimensions dimension = {};
		TextureType type = TextureType::Type_2D;
		VkFormat format = VK_FORMAT_UNDEFINED;
		SampleCount samples = SampleCount::X1;
		uint8_t usage = {};
		StorageType storage = StorageType::Device;

		uint32_t numMipLevels = 1;
		uint32_t numLayers = 1;
		ComponentMapping components = {};

		const void* initialData = nullptr;
		uint32_t dataNumMipLevels = 1; // how many mip-levels we want to create & fill in when uploading data
		bool generateMipmaps = false; // works only if initialData is not nulll
		const char* debugName = "Unnamed Texture";
	};

	struct TextureViewSpec {
		VkImageViewType type = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
		uint32_t layer = 0;
		uint32_t numLayers = 1;
		uint32_t mipLevel = 0;
		uint32_t numMipLevels = 1;
		ComponentMapping components = {};
		const char* debugName = "Unnamed Texture View";
	};
	struct ShaderSpec {
		std::filesystem::path filePath;
		const char* debugName = "Unnamed Shader";
	};

}