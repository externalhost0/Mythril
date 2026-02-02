import GPUStructs;

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
    float4 Tangent : TANGENT;
    float4 LightSpacePos;
};

struct PushConstant {
    float4x4 model;
    Ptr<Vertex> vba;
}
[[vk::push_constant]]
PushConstant push;

ParameterBlock<FrameData> frame;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = push.vba[input.VertexID];

    float3 worldpos = mul(push.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(frame.camera.projView, float4(worldpos, 1));

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    output.FragColor = float4(1, 0, 0, 1);
    return output;
}