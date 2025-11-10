#include "GPUStructs.h"

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};

[[vk::push_constant]]
GaussianPushConstant push;

// [[SpecializationConstant]] 
// const int blurdirection = 0;


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

    float weight[5];
    weight[0] = 0.227027;
    weight[1] = 0.1945946;
    weight[2] = 0.1216216;
    weight[3] = 0.054054;
    weight[4] = 0.016216;

    float2 textureSize;
    push.emissiveTexture.GetDimensions(textureSize.x, textureSize.y);
    textureSize = { 1280*2, 720*2 };
    float2 tex_offset = 1.0 / textureSize * push.scale;                                    // gets size of single texel
    float3 result = push.emissiveTexture.Sample(push.samplerId, input.UV).rgb * weight[0]; // current fragment's contribution
    [[unroll]]
    for (int i = 1; i < 5; ++i) {
        if (push.blurdirection == 1) {
            // H
            result += push.emissiveTexture.Sample(push.samplerId, input.UV + float2(tex_offset.x * i, 0.0)).rgb * weight[i] * push.scale;
            result += push.emissiveTexture.Sample(push.samplerId, input.UV - float2(tex_offset.x * i, 0.0)).rgb * weight[i] * push.scale;
        } else {
            // V
            result += push.emissiveTexture.Sample(push.samplerId, input.UV + float2(0.0, tex_offset.y * i)).rgb * weight[i] * push.scale;
            result += push.emissiveTexture.Sample(push.samplerId, input.UV - float2(0.0, tex_offset.y * i)).rgb * weight[i] * push.scale;
        }
    }
    if (push.blurdirection == 0) {
        output.FragColor = float4((result * push.intensity) + push.colorTexture.Sample(push.samplerId, input.UV).rgb, 1);
    } else {
        output.FragColor = float4(result * push.intensity, 1);
    }
    return output;
}