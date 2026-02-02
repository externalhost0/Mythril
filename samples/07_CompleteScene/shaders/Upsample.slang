import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D> inTexture;
    DescriptorHandle<RWTexture2D> outTexture;
    DescriptorHandle<SamplerState> sampler;
    int2 outSize;
    float filterRadius;
    float pad;
}

[vk::push_constant]
PushConstants push;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 threadId : SV_DispatchThreadID) {
    uint2 outputCoord = threadId.xy;

    if (outputCoord.x >= uint(push.outSize.x) ||
        outputCoord.y >= uint(push.outSize.y)) {
        return;
    }
    float2 uv = (float2(outputCoord) + 0.5) / push.outSize;

    float2 dy = float2(0, push.filterRadius);
    float2 dx = float2(push.filterRadius, 0);


    // Take 9 samples with tent filter pattern:
    // a - b - c
    // d - e - f
    // g - h - i

    float3 a = push.inTexture.SampleLevel(push.sampler, uv - dx + dy, 0).rgb;
    float3 b = push.inTexture.SampleLevel(push.sampler, uv + dy, 0).rgb;
    float3 c = push.inTexture.SampleLevel(push.sampler, uv + dx + dy, 0).rgb;

    float3 d = push.inTexture.SampleLevel(push.sampler, uv - dx, 0).rgb;
    float3 e = push.inTexture.SampleLevel(push.sampler, uv, 0).rgb;
    float3 f = push.inTexture.SampleLevel(push.sampler, uv + dx, 0).rgb;

    float3 g = push.inTexture.SampleLevel(push.sampler, uv - dx - dy, 0).rgb;
    float3 h = push.inTexture.SampleLevel(push.sampler, uv - dy, 0).rgb;
    float3 i = push.inTexture.SampleLevel(push.sampler, uv + dx - dy, 0).rgb;

    // Apply 3x3 tent filter weights:
    //  1   | 1 2 1 |
    // -- * | 2 4 2 |
    // 16   | 1 2 1 |
    float3 result = e * 4.0;
    result += (b + d + f + h) * 2.0;
    result += (a + c + g + i);
    result *= 1.0 / 16.0;

    float3 prev = push.outTexture[outputCoord].rgb;
    float3 curr = result;
    push.outTexture[outputCoord] = float4(prev+curr, 1);
}