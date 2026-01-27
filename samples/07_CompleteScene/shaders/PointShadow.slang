import GPUStructs;

struct VSInput {
    uint VertexID : SV_VulkanVertexID;
    uint InstanceID : SV_VulkanInstanceID;
    uint ViewID : SV_ViewID;
};
struct v2f {
    float4 ClipPos : SV_Position;
    float3 WorldPos : POSITION;
};

struct PushConstant {
    Ptr<float4x4> matrices;
    Ptr<Vertex> vba;
    float4x4 model;
    uint nLight;
    float3 lightPos;
}
[[vk::push_constant]]
PushConstant push;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;
    Vertex v = push.vba[input.VertexID];
    float4x4 mvp = push.matrices[push.nLight * 6 + input.ViewID];
    output.ClipPos = mul(mvp, float4(v.position, 1));
    output.WorldPos = mul(push.model, float4(v.position, 1)).xyz;
    return output;
}

// the fragment/pixel shader for a depth only pass is irrelevant
[shader("pixel")]
float fs_main(v2f input) : SV_Depth {
    float distance = length(input.WorldPos - push.lightPos);
    // Normalize to [0, 1] using far plane
    float farPlane = 60.0; // Must match pointlight_farplane
    return distance / farPlane;
}