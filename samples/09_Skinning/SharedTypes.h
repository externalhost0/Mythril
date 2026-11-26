#pragma once

// sample 09 shares GPUStructs.h between C++ and Slang, so its C++ side spells Slang's resource types here
#ifdef __cplusplus
#include <concepts>
#include <cstdint>

#include "shader_types.h"

#define NAMESPACE_BEGIN() namespace mythril::GPU {
#define NAMESPACE_END() }

NAMESPACE_BEGIN()

template<typename T>
struct Ptr {
	uint64_t addr;
	Ptr(uint64_t addr) :
		addr(addr) {}
};

template<typename T>
struct DescriptorHandle {
	uint64_t index{}; // uint2 in slang
	DescriptorHandle(uint32_t index) :
		index(index) {}
	template<std::integral U> requires (sizeof(U) == 8)
	DescriptorHandle(U) { static_assert(sizeof(U) == 0, "DescriptorHandle takes a bindless index (.index())"); }
};

template<typename T>
struct StructuredBuffer {
	T* buf;
};

template<typename T>
struct ConstantBuffer {
	T* buf;
};

struct Texture2D {
};
template<typename T>
struct RWTexture2D {
};
struct SamplerState {
};
struct RaytracingAccelerationStructure {
};

template<>
struct DescriptorHandle<RaytracingAccelerationStructure> {
	uint64_t address;
	DescriptorHandle(uint64_t address) :
		address(address) {}
	DescriptorHandle(uint32_t) = delete;
};

NAMESPACE_END()

#elif __SLANG__

#define NAMESPACE_BEGIN()
#define NAMESPACE_END()

#else
#error "Unknown Language Environment!"
#endif
