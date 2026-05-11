
#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    // system defined
    float4 ClipPos          : SV_Position;
    // user defined
    float3 Normal           : NORMAL;
    float2 UV               : TEXCOORD0;
    float3 WorldPosition    : POSITION;
};

[vk::push_constant]
GeometryPushConstant perObject;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    float3 worldpos = mul(perObject.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(mul(perObject.perFrame.camera.proj, perObject.perFrame.camera.view), float4(worldpos, 1));

    output.Normal = mul(perObject.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float2 resolution = perObject.perFrame.renderResolution;
    float time = perObject.perFrame.time;

    float2 fragCoord = input.UV * resolution;
    float2 uv = (2.0 * fragCoord - resolution) / min(resolution.x, resolution.y);
    
    float distort = perObject.perMaterial.distortAmount;
    for (float i = perObject.perMaterial.warpIterations; i < 10.0; i += 1.0) {
        uv.x += distort / i * cos(i * 2.5 * uv.y + time);
        uv.y += distort / i * cos(i * 1.5 * uv.x + time);
    }

    float glow = 0.1 / abs(sin(time - uv.y - uv.x));
    glow *= perObject.perMaterial.glowAmount;
    float3 color = glow.xxx * perObject.perMaterial.tint;
    output.FragColor = float4(color, 1.f);

    return output;
}