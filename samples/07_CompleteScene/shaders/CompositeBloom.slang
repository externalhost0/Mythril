#include "GPUStructs.h"

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
[vk::push_constant]
CompositePushConstant push;

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float4 sceneColor = push.sceneTexture.Sample(push.sampler, input.UV);
    float4 glowColor = push.blurredTexture.Sample(push.sampler, input.UV);
    output.FragColor = sceneColor + (glowColor * push.intensity);

    return output;
}
