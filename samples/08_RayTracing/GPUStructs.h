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

NAMESPACE_END()
