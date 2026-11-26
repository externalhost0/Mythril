#pragma once

#include "ObjectHandles.h"
#include "Objects.h"
#include "RenderGraphDescriptions.h"

#include <volk.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace mythril {
	template<class Tag>
	class TextureSlot {
	public:
		TextureSlot() = default;
		TextureSlot(const ResolvedTexture& texture) : _index(texture.index()) {}
		TextureSlot(const Texture& texture) : _index(texture.index()) {}
		TextureSlot(TextureHandle texture) : _index(texture.index()) {}
	private:
		// DescriptorHandle lowers to uint2
		uint32_t _index = 0;
		uint32_t _unused = 0;
	};
	using SampledTextureSlot = TextureSlot<struct SampledTag>;
	using StorageTextureSlot = TextureSlot<struct StorageTag>;

	class SamplerSlot {
	public:
		SamplerSlot() = default;
		SamplerSlot(const Sampler& sampler) : _index(sampler.index()) {}
		SamplerSlot(SamplerHandle sampler) : _index(sampler.index()) {}
	private:
		uint32_t _index = 0;
		uint32_t _unused = 0;
	};

	// what CTX::gpuAddress returns, so a push struct field takes it without restating the pointee type
	struct DeviceAddress {
		VkDeviceAddress value = 0;
		constexpr operator VkDeviceAddress() const { return value; }
	};

	class AccelerationStructureSlot {
	public:
		AccelerationStructureSlot() = default;
		AccelerationStructureSlot(const DeviceAddress address) : AccelerationStructureSlot(address.value) {}
		explicit AccelerationStructureSlot(VkDeviceAddress address) : _low(static_cast<uint32_t>(address)), _high(static_cast<uint32_t>(address >> 32)) {}
	private:
		// DescriptorHandle<RaytracingAccelerationStructure> lowers to uint2 holding the address
		uint32_t _low = 0;
		uint32_t _high = 0;
	};

	template<class T>
	class DevicePtr {
	public:
		DevicePtr() = default;
		DevicePtr(const DeviceAddress address) : _address(address.value) {}
		explicit DevicePtr(VkDeviceAddress address) : _address(address) {}
	private:
		VkDeviceAddress _address = 0;
	};

	namespace detail {
		template<class Matrix>
		constexpr size_t MatrixVectorBytes(const size_t unindexed) {
			if constexpr (requires(Matrix& m) { m[0]; })
				return sizeof(std::remove_reference_t<decltype(std::declval<Matrix&>()[0])>);
			else
				return unindexed;
		}
	} // namespace detail

	template<class T>
	concept GeneratedPushStruct = std::is_trivially_copyable_v<T> && requires {
		{ T::kLayoutHash } -> std::convertible_to<uint64_t>;
	};
} // namespace mythril
