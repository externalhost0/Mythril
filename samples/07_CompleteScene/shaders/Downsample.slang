import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D> srcTexture;
    DescriptorHandle<RWTexture2D> outTexture;
    DescriptorHandle<SamplerState> sampler;
    uint32_t mipLevel;
    float pad;
}

[vk::push_constant]
PushConstants push;

float3 PowVec3(float3 v, float p) {
    return float3(pow(v.x, p), pow(v.y, p), pow(v.z, p));
}

static const float invGamma = 1.0 / 2.2;
float3 ToSRGB(float3 v) {
    return PowVec3(v, invGamma);
}

float sRGBToLuma(float3 col) {
    return dot(col, float3(0.299, 0.587, 0.114));
}

float KarisAverage(float3 col) {
    // Formula is 1 / (1 + luma)
    float luma = sRGBToLuma(ToSRGB(col)) * 0.25;
    return 1.0 / (1.0 + luma);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 threadId : SV_DispatchThreadID) {
    // Query texture dimensions
    uint2 dstSize;
    push.outTexture.GetDimensions(dstSize.x, dstSize.y);

    uint2 outputCoord = threadId.xy;

    if (outputCoord.x >= dstSize.x || outputCoord.y >= dstSize.y) {
        return;
    }

    // Query source texture dimensions
    uint srcWidth, srcHeight;
    push.srcTexture.GetDimensions(srcWidth, srcHeight);
    float2 srcResolution = float2(srcWidth, srcHeight);

    // Calculate normalized texture coordinates
    float2 uv = (float2(outputCoord) + 0.5) / float2(dstSize);
    float2 srcTexelSize = 1.0 / srcResolution;
    float x = srcTexelSize.x;
    float y = srcTexelSize.y;

    // Take 13 samples around current texel:
    // a - b - c
    // - j - k -
    // d - e - f
    // - l - m -
    // g - h - i
    // === ('e' is the current texel) ===
    SamplerState linearSampler = push.sampler;

    float3 a = push.srcTexture.SampleLevel(linearSampler, uv + float2(-2 * x, 2 * y), 0).rgb;
    float3 b = push.srcTexture.SampleLevel(linearSampler, uv + float2(0, 2 * y), 0).rgb;
    float3 c = push.srcTexture.SampleLevel(linearSampler, uv + float2(2 * x, 2 * y), 0).rgb;

    float3 d = push.srcTexture.SampleLevel(linearSampler, uv + float2(-2 * x, 0), 0).rgb;
    float3 e = push.srcTexture.SampleLevel(linearSampler, uv + float2(0, 0), 0).rgb;
    float3 f = push.srcTexture.SampleLevel(linearSampler, uv + float2(2 * x, 0), 0).rgb;

    float3 g = push.srcTexture.SampleLevel(linearSampler, uv + float2(-2 * x, -2 * y), 0).rgb;
    float3 h = push.srcTexture.SampleLevel(linearSampler, uv + float2(0, -2 * y), 0).rgb;
    float3 i = push.srcTexture.SampleLevel(linearSampler, uv + float2(2 * x, -2 * y), 0).rgb;

    float3 j = push.srcTexture.SampleLevel(linearSampler, uv + float2(-x, y), 0).rgb;
    float3 k = push.srcTexture.SampleLevel(linearSampler, uv + float2(x, y), 0).rgb;
    float3 l = push.srcTexture.SampleLevel(linearSampler, uv + float2(-x, -y), 0).rgb;
    float3 m = push.srcTexture.SampleLevel(linearSampler, uv + float2(x, -y), 0).rgb;

    float3 result;

    // Apply weighted distribution with optional Karis average
    // 0.125*5 + 0.03125*4 + 0.0625*4 = 1
    if (push.mipLevel == 0) {
        // Writing to mip 0: apply Karis average to each block of 4 samples
        // to prevent fireflies (very bright subpixels that cause pulsating artifacts)
        float3 groups[5];
        groups[0] = (a + b + d + e) * (0.125 / 4.0);
        groups[1] = (b + c + e + f) * (0.125 / 4.0);
        groups[2] = (d + e + g + h) * (0.125 / 4.0);
        groups[3] = (e + f + h + i) * (0.125 / 4.0);
        groups[4] = (j + k + l + m) * (0.5 / 4.0);

        groups[0] *= KarisAverage(groups[0]);
        groups[1] *= KarisAverage(groups[1]);
        groups[2] *= KarisAverage(groups[2]);
        groups[3] *= KarisAverage(groups[3]);
        groups[4] *= KarisAverage(groups[4]);

        result = groups[0] + groups[1] + groups[2] + groups[3] + groups[4];
        result = max(result, 0.0001);
    }
    else {
        // Standard weighted downsample for other mip levels
        result = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    push.outTexture[outputCoord] = float4(result, 1);
}