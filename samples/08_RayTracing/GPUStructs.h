//
// Created by Hayden Rivas on 7/3/26.
//

#pragma once

#include "mythril/shader_types.h"

NAMESPACE_BEGIN()

struct Vertex {
	float3 position;
};
struct PushConstant {
	float4x4 mvp;
	Ptr<Vertex> vertexBufferAddress;
};

struct PerFrame {
	float4x4 viewInverse;
	float4x4 projInverse;
};
struct ShadowPushConstant {
	Ptr<PerFrame> frame;
    float4 lightDir;
    Ptr<Vertex> vertices;
	Ptr<uint32_t> indices;
    DescriptorHandle<RWTexture2D<float4>> outTexture;
    DescriptorHandle<RaytracingAccelerationStructure> tlas;
    uint32_t enableShadows;
    uint32_t debugMode;
};

NAMESPACE_END()
