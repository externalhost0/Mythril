#include "GPUStructs.h"

struct v2f
{
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput
{
    float4 FragColor : SV_Target0;
};

[vk::push_constant]
EmissiveExtractPushConstant push;

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float3 color = push.sceneTexture.Sample(push.sampler, input.UV).rgb;

    float luminance = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0.0);

    float knee = push.threshold * push.softKnee;
    float softMin = push.threshold - knee;

    float softness = saturate((luminance - softMin) / (2.0 * knee));
    softness = softness * softness;

    float bloomAmount = max(luminance - softMin, 0.0) * softness;
    float3 emissive = color * bloomAmount;

    output.FragColor = float4(emissive, 1.0);
    return output;
}
