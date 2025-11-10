// import "Common.Math.slang";

export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handleValue) {
    return defaultGetDescriptorFromHandle(handleValue, BindlessDescriptorOptions.None);
}

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
struct Vertex {
    float3 position;
    float uv_x;
    float3 normal;
    float uv_y;
    float4 tangent;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float3 position;
}
struct GlobalData {
    CameraData camera;
    float2 resolution;
    float time;
}
// struct MaterialData {
//     float3 tint;
//     float warpIterations;
//     float glowAmount;
//     float distortAmount;
// }

// ParameterBlock<MaterialData> perMaterial;
// ParameterBlock<GlobalData> perFrame;

struct PushConstantData {
    float4x4 model;
    Ptr<Vertex> vertexBufferAddress;
    DescriptorHandle<Texture2D> tex;
    DescriptorHandle<SamplerState> sampler;
}
[vk::push_constant]
PushConstantData perObject;

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
    
    output.FragColor = float4(perObject.tex.Sample(perObject.sampler, input.UV).rgb, 1);
    return output;
}