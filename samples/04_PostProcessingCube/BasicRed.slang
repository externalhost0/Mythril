#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    float4 ClipPos : SV_Position;
};

[[vk::push_constant]]
GeometryPushConstant perObject;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    output.ClipPos = float4(mul(perObject.mvp, float4(v.position, 1)));

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    output.FragColor = float4(1.0, 0.0, 0.0, 1.0);

    return output;
}