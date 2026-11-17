//
// Created by Hayden Rivas on 11/17/26.
//

#pragma once

#include <mythril/shader_types.h>

NAMESPACE_BEGIN()

static const uint kSkinLinear   = 0;
static const uint kSkinDualQuat = 1;
static const uint kSkinNone     = 2;

static const uint kViewLit       = 0;
static const uint kViewWeights   = 1;
static const uint kViewInfluence = 2;

struct SkinnedVertex {
	float3 position;
	float _pad0;
	float3 normal;
	float _pad1;
	uint4 joints;
	float4 weights;
};

struct DualQuat {
	float4 real;
	float4 dual;
};

struct FrameData {
	float4x4 viewProj;
	float3 lightDir;
	float _pad;
};

struct SkinPushConstant {
	float4x4 model;
	Ptr<SkinnedVertex> vertices;
	Ptr<FrameData> frame;
	Ptr<float4x4> jointMatrices;
	Ptr<DualQuat> jointDualQuats;
	uint skinningMode;
	uint viewMode;
	uint selectedJoint;
	uint _pad;
};

NAMESPACE_END()
