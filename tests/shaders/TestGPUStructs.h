#pragma once

#include "mythril/shader_types.h"

NAMESPACE_BEGIN()

struct TestVertex {
    float3 position;
};

struct TestPushConstant {
    float4x4 mvp;
    Ptr<TestVertex> vertices;
};

struct SimplePush {
    float4 color;
};

struct ComputePush {
    Ptr<uint> outputBuffer;
    uint count;
    uint _pad;
};

NAMESPACE_END()
