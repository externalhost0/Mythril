
#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
    float4 EmissiveTarget : SV_Target1;
};
struct v2f {
    // system defined
    float4 ClipPos          : SV_Position;
    // user defined
    float3 Normal           : NORMAL;
    float2 UV               : TEXCOORD0;
    float3 WorldPosition    : POSITION;
};

// man why do i even try
// https://github.com/shader-slang/slang/issues/6991
// because there is no compiler option to shift parameter block set index,
// you will unfortunately HAVE to manually annotate your parameters
// this is solely so that our bindless set that should always exist can exist in every shader

// our parameters
// i honestly dont think you ever need more than 2 user defined sets
ParameterBlock<GlobalData> perFrame;
ParameterBlock<MaterialData> perMaterial;

[vk::push_constant]
GeometryPushConstant perObject;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;


    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    output.ClipPos = float4(mul(perObject.model, float4(v.position, 1)));
    // float3 worldpos = mul(perObject.model, float4(v.position, 1)).xyz;
    // output.ClipPos = mul(mul(perFrame.camera.proj, perFrame.camera.view), float4(worldpos, 1));

    output.Normal = mul(perObject.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    // output.WorldPosition = worldpos;

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float2 resolution = perFrame.renderResolution;
    float time = perFrame.time;

    float2 fragCoord = input.UV * resolution;
    float2 uv = (2.0 * fragCoord - resolution) / min(resolution.x, resolution.y);
    
    float distort = perMaterial.distortAmount;
    for (float i = perMaterial.warpIterations; i < 10.0; i += 1.0) {
        uv.x += distort / i * cos(i * 2.5 * uv.y + time);
        uv.y += distort / i * cos(i * 1.5 * uv.x + time);
    }

    float glow = 0.1 / abs(sin(time - uv.y - uv.x));
    glow *= perMaterial.glowAmount;
    float3 color = glow.xxx * perMaterial.tint;
    output.FragColor = float4(color, 1.f);

    float brightness = dot(color, float3(0.2126, 0.7152, 0.0722));
    float threshold = 0.9f; // e.g., 1.0 or configurable
    float3 emissive = brightness > threshold ? color : float3(0.0, 0.0, 0.0);
    output.EmissiveTarget = float4(emissive, 1);

    return output;
}