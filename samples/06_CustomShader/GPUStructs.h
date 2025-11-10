//
// Created by Hayden Rivas on 11/9/25.
//

#pragma once

// REQUIRED BY SHARED HEADER FILES BETWEEN SLANG & C++
#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

struct GaussianPushConstant {
	DescriptorHandle<Texture2D> colorTexture;
	DescriptorHandle<Texture2D> emissiveTexture;
	DescriptorHandle<SamplerState> samplerId;
	float scale;
	float intensity;
	int blurdirection;
};

struct Vertex {
	float3 position;
	float uv_x;
	float3 normal;
	float uv_y;
	float4 tangent;
	// we want easy to use constructors in our cpp code we just preprocessor this
#ifdef __cplusplus
		Vertex() = default;
		Vertex(glm::vec3 pos) : position(pos) {};
		Vertex(glm::vec3 pos, glm::vec3 norm, glm::vec2 uv) : position(pos), normal(norm), uv_x(uv.x), uv_y(uv.y) {};
#endif
};

struct GeometryPushConstant {
	float4x4 model;
	Ptr<Vertex> vertexBufferAddress;
};

struct CameraData {
	float4x4 proj;
	float4x4 view;
	float3 position;
};

struct GlobalData {
	CameraData camera;
	float2 resolution;
	float time;
};
struct MaterialData {
	float3 tint;
	float warpIterations;
	float glowAmount;
	float distortAmount;
};

NAMESPACE_END()