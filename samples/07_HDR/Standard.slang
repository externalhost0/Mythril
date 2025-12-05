#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    float4 ClipPos : SV_Position;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float3 WorldPosition : POSITION;
};

[[vk::push_constant]]
GeometryPushConstants push;

ParameterBlock<FrameData> frame;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = push.vba[input.VertexID];

    float3 worldpos = mul(push.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(mul(frame.camera.proj, frame.camera.view), float4(worldpos, 1));

    output.Normal = mul(push.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    return output;
}

float linearizeDepth(float vertexZ, CameraData camera) {
    float ndc = vertexZ; // already in [-1,1]
    float linearDepth = (2.0 * camera.near * camera.far) / (camera.far + camera.near - ndc * (camera.far - camera.near));
    return linearDepth / camera.far;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float4 baseColor = push.tintColor;
    baseColor *= push.baseColorTexture.Sample(push.samplerState, input.UV);
    output.FragColor = baseColor;
    output.FragColor = baseColor + float4(input.UV, 0, 1);

    return output;
}