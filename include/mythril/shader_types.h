//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once

#ifdef __cplusplus // when this header is processed in cpp
#include <glm/glm.hpp>
#include <slang/slang-cpp-types.h>

#define NAMESPACE_BEGIN() namespace GPU {
#define NAMESPACE_END() }


NAMESPACE_BEGIN()

using float2x2 = glm::mat2;
using float2x3 = glm::mat2x3;
using float3x2 = glm::mat3x2;
using float3x3 = glm::mat3;
using float3x4 = glm::mat3x4;
using float4x3 = glm::mat4x3;
using float4x4 = glm::mat4;

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;

using int2 = glm::ivec2;
using int3 = glm::ivec3;
using int4 = glm::ivec4;

using uint  = unsigned int;
using uint2 = glm::uvec2;
using uint3 = glm::uvec3;
using uint4 = glm::uvec4;


template<typename T>
struct Ptr {
	VkDeviceAddress addr;
	Ptr(VkDeviceAddress addr) : addr(addr) {}
};

template<typename T>
struct DescriptorHandle {
	uint64_t index; // uint2 in slang
	DescriptorHandle(uint64_t index) : index(index) {}
};

template<typename T>
struct StructuredBuffer {
	T* buf;
};

template<typename T>
struct ConstantBuffer {
	T* buf;
};

struct Texture2D;
struct SamplerState;


NAMESPACE_END()

#elif __SLANG__ // when this header is processed in slang

#define NAMESPACE_BEGIN()
#define NAMESPACE_END()

// will be injected into shaders that use a header file for struct definitions
export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handleValue) {
    return defaultGetDescriptorFromHandle(handleValue, BindlessDescriptorOptions.None);
}

#else // when this header is processed in an unknown languages
#error "Unknown Language Environment!"
#endif