#include "GPUStructs.h"

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};

[[vk::push_constant]]
GaussianPushConstant push;


struct FSOutput {
    float4 FragColor : SV_Target0;
};

// GaussianBlur.slang - fragment
static inline float gaussian(float x, float sigma) {
    return exp(-0.5 * (x * x) / (sigma * sigma));
}

[[shader("pixel")]]
FSOutput fs_main(v2f input)
{
    FSOutput output;

    // read source dims (the texture we're sampling from)
    uint srcW, srcH;
    push.emissiveTexture.GetDimensions(srcW, srcH);
    float2 srcSize = float2(srcW, srcH);

    // protect against bad sizes
    if (srcSize.x <= 0.0 || srcSize.y <= 0.0) {
        output.FragColor = float4(0, 0, 0, 1);
        return output;
    }

    // texel in UV space for a single texel of the source
    float2 texel = 1.0 / srcSize;

    // interpret push.scale as pixel radius (clamped)
    int radius = clamp((int)round(push.scale), 1, 32);
    float sigma = max(0.1, push.scale * 0.5);

    // pick direction
    float2 dir = (push.blurdirection == 1) ? float2(1.0, 0.0) : float2(0.0, 1.0);

    // center sample
    float3 accum = float3(0, 0, 0);
    float wsum = 0.0;

    float w0 = gaussian(0.0, sigma);
    accum += push.emissiveTexture.Sample(push.sampler, input.UV).rgb * w0;
    wsum += w0;

    // symmetric taps
    for (int i = 1; i <= radius; ++i) {
        float w = gaussian((float)i, sigma);
        float2 off = dir * (float(i) * texel);
        accum += push.emissiveTexture.Sample(push.sampler, input.UV + off).rgb * w;
        accum += push.emissiveTexture.Sample(push.sampler, input.UV - off).rgb * w;
        wsum += w * 2.0;
    }

    float3 blurred = accum / max(1e-6, wsum);
    output.FragColor = float4(blurred, 1.0);
    return output;
}
