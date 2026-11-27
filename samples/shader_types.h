//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once

// the C++ types that the generated shader headers use for Slang's vector and matrix types
#include <glm/glm.hpp>

namespace mythril::GPU {
    using float2x2 = glm::mat2;
    using float2x3 = glm::mat3x2;
    using float3x2 = glm::mat2x3;
    using float3x3 = glm::mat3;
    using float3x4 = glm::mat4x3;
    using float4x3 = glm::mat3x4;
    using float4x4 = glm::mat4;

    using float2 = glm::vec2;
    using float3 = glm::vec3;
    using float4 = glm::vec4;

    using int2 = glm::ivec2;
    using int3 = glm::ivec3;
    using int4 = glm::ivec4;

    using uint = unsigned int;
    using uint2 = glm::uvec2;
    using uint3 = glm::uvec3;
    using uint4 = glm::uvec4;
} // namespace mythril::GPU
