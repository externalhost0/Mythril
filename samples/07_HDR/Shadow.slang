import GPUStructs;

struct VSInput {
    uint VertexID : SV_VertexID;
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