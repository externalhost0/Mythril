import GPUStructs;

struct VSInput {
    uint VertexID : SV_VulkanVertexID;
    uint InstanceID : SV_VulkanInstanceID;
};
struct v2f {
    float4 ClipPos : SV_Position;
};

struct PushConstant {
    Ptr<float4x4> matrices;
    Ptr<Vertex> vba;
    int nLight;
    float pad;
}
[[vk::push_constant]]
PushConstant push;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;
    Vertex v = push.vba[input.VertexID];
    float4x4 mvp = push.matrices[push.nLight * 6 + input.InstanceID];
    output.ClipPos = mul(mvp, float4(v.position, 1));
    return output;
}

// the fragment/pixel shader for a depth only pass is irrelevant
[shader("pixel")]
void fs_main(v2f input) {}