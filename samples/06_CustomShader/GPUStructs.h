//
// Created by Hayden Rivas on 11/9/25.
//

#pragma once

// REQUIRED BY SHARED HEADER FILES BETWEEN SLANG & C++
#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

// must be a multiple of 8 because of largest member (sizeof(DescriptorHandle<T>) == 8 so extra padding of 4 bytes is added for 40 bytes
struct GaussianPushConstant {
	DescriptorHandle<Texture2D> colorTexture; // 8
	DescriptorHandle<Texture2D> emissiveTexture; // 8
	DescriptorHandle<SamplerState> samplerId; // 8 = 24
	float scale; // 4
	float intensity; // 4
	int blurdirection; // 4 = 12
	// 12+24 = 36
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

struct ObjectData {
	float3 position;
	float3 color;
};
struct GlobalData {
	Ptr<ObjectData> objects;
	CameraData camera;
	float2 renderResolution;
	float time;
};
struct MaterialData {
	float3 tint;
	float warpIterations;
	float glowAmount;
	float distortAmount;
};

NAMESPACE_END()