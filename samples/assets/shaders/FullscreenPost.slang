import "Common.Math";
import "Common.Sets";

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV      : TEXCOORD0;
};

struct PushConstant {
    uint imageTexId;
    float time;
}
[[vk::push_constant]]
PushConstant push;

[shader("vertex")]
v2f vs_main(uint VertexID : SV_VertexID) {
    v2f output;
    // generates the fullscreen triangle without any cpu interaction
    // https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html

    output.UV = float2((VertexID << 1) & 2, VertexID & 2 );
    output.ClipPos = float4(output.UV * 2.0 + -1.0, 0.0, 1.0);
    // flip the y axis
    output.UV.y = 1.0 - output.UV.y;
    return output;
}

struct FSOutput {
    float4 FragColor : SV_Target0;
};

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float time = push.time;
    float2 uv = input.UV;
    float shift = sin((uv.y + time) * 20.0) * 0.01;


    float3 col;
    col.r = textureBindless2D(push.imageTexId, 0, input.UV + shift).r;
    col.g = textureBindless2D(push.imageTexId, 0, input.UV).g;
    col.b = textureBindless2D(push.imageTexId, 0, input.UV - shift).b;
    
    output.FragColor = float4(col, 1);


    return output;
}