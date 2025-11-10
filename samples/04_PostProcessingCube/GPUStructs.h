//
// Created by Hayden Rivas on 11/9/25.
//

#pragma once

#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

struct Vertex {
	float3 position;
};
struct GeometryPushConstant {
	float4x4 mvp;
	Ptr<Vertex> vertexBufferAddress;
};

struct FullscreenPushConstant {
	DescriptorHandle<Texture2D> colorTexture;
	DescriptorHandle<SamplerState> linearSampler;
	float time;
};

NAMESPACE_END()