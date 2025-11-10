// #include "Common.Sets.slang"

// MANDATORY SNIPPET OF CODE, REQUIRED FOR OVERRIDING BINDLESS BEHAVIOR WITH SLANG
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

struct Environment {
    uint backgroundId;
}
struct ObjectData {
    float4x4 model;
    float3 color;
}
struct GlobalData {
    CameraData camera;
    float2 resolution;
    float time;
}

// unfortunately will have to do this until they add descriptor handle reflection support
// https://github.com/shader-slang/slang/issues/8845
enum HandleType {
    //sampled images
	Texture1D,
    Texture2D,
	Texture3D,
	TextureCube,

	// samplers
	SamplerState,

	// storage images
	RWTexture1D,
	RWTexture2D,
    RWTexture3D,

	Unknown
}
[__AttributeUsage(_AttributeTargets.Var)]
struct MarkHandleAttribute {
    HandleType type;
};

struct MaterialData {
    float3 tint;
    float roughness;
    float normals;
    float opacity;
}

ParameterBlock<MaterialData> perMaterial;
ParameterBlock<GlobalData> perFrame;

struct PushConstantData {
    float4x4 model;
    Ptr<Vertex> vertexBufferAddress;
    [MarkHandle(HandleType::Texture2D)] DescriptorHandle<Texture2D> tex;
    [MarkHandle(HandleType::SamplerState)] DescriptorHandle<SamplerState> sampler;
}

[vk::push_constant]
PushConstantData perObject;


[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    float3 worldpos = mul(perObject.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(mul(perFrame.camera.proj, perFrame.camera.view), float4(worldpos, 1));

    output.Normal = mul(perObject.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float4 texture = perObject.tex.Sample(perObject.sampler, input.UV);

    float roughness = perMaterial.roughness;
    float normals   = perMaterial.normals;
    float ambient   = 0.1f;

    float3 finalColor = texture.rgb + perMaterial.tint.rgb;
    output.FragColor = float4(finalColor+perMaterial.roughness, perMaterial.opacity);
    
    return output;
}