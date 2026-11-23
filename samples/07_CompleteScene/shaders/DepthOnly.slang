import GPUStructs;

// note that we use SV_VulkanVertexID instead of the typical SV_VertexID
// this is just because SV_VertexID doesnt include the first vertex
// https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html#using-sv-instanceid-and-sv-vertexid-with-spir-v-target
struct VSInput {
    uint VertexID : SV_VulkanVertexID;
};
struct v2f {
    float4 ClipPos : SV_Position;
};

struct PushConstant {
    float4x4 mvp;
    Ptr<Vertex> vba;
}
[[vk::push_constant]]
PushConstant push;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;
    Vertex v = push.vba[input.VertexID];
    output.ClipPos = mul(push.mvp, float4(v.position, 1));
    return output;
}

// the fragment/pixel shader for a depth only pass is irrelevant
[shader("pixel")]
void fs_main(v2f input) {}