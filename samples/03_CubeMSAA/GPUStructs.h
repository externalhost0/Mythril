//
// Created by Hayden Rivas on 11/9/25.
//

#pragma once

#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

struct Vertex {
	float3 position;
};
struct PushConstant {
	float4x4 mvp;
	Ptr<Vertex> vertexBufferAddress;
};

NAMESPACE_END()