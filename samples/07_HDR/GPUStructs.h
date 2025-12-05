//
// Created by Hayden Rivas on 12/2/25.
//

#pragma once
#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

struct Vertex {
	float3 position;
	float uv_x;
	float3 normal;
	float uv_y;
	float4 tangent;
#ifdef __cplusplus
	Vertex() = default;
	Vertex(glm::vec3 pos) : position(pos) {};
	Vertex(glm::vec3 pos, glm::vec3 norm, glm::vec2 uv) : position(pos), normal(norm), uv_x(uv.x), uv_y(uv.y) {};
#endif
};

struct GeometryPushConstants {
	float4x4 model;
	Ptr<Vertex> vba;
    float4 tintColor;
	DescriptorHandle<Texture2D> baseColorTexture;
	DescriptorHandle<SamplerState> samplerState;
};
struct BlurPushConstants {

};

struct CameraData {
	float4x4 proj;
	float4x4 view;
	float3 position;
    float far;
    float near;
};
struct FrameData {
    CameraData camera;
};


NAMESPACE_END()