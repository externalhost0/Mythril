#include "TestGPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct v2f {
    float4 ClipPos : SV_Position;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};

[[vk::push_constant]]
TestPushConstant push;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;
    TestVertex v = push.vertices[input.VertexID];
    output.ClipPos = mul(push.mvp, float4(v.position, 1.0));
    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    output.FragColor = float4(1.0, 0.0, 0.0, 1.0);
    return output;
}
