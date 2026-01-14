import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D> inTexture;
    DescriptorHandle<RWTexture2D> outTexture;
    DescriptorHandle<SamplerState> sampler;
    float filterRadius;
    float blend;
}

[vk::push_constant]
PushConstants push;


[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 threadId : SV_DispatchThreadID) {
    uint2 outputCoord = threadId.xy;

    int2 outSize;
    int2 inSize;
    push.outTexture.GetDimensions(outSize.x, outSize.y);
    push.inTexture.GetDimensions(inSize.x, inSize.y);

    if (outputCoord.x >= uint(outSize.x) ||
        outputCoord.y >= uint(outSize.y)) {
        return;
    }

    // Calculate normalized texture coordinates for the output pixel
    float2 uv = (float2(outputCoord) + 0.5) / outSize;

    float x = push.filterRadius;
    float y = push.filterRadius;

    // Take 9 samples with tent filter pattern:
    // a - b - c
    // d - e - f
    // g - h - i
    
    float3 a = push.inTexture.SampleLevel(push.sampler, uv + float2(-x, y), 0).rgb;
    float3 b = push.inTexture.SampleLevel(push.sampler, uv + float2(0, y), 0).rgb;
    float3 c = push.inTexture.SampleLevel(push.sampler, uv + float2(x, y), 0).rgb;

    float3 d = push.inTexture.SampleLevel(push.sampler, uv + float2(-x, 0), 0).rgb;
    float3 e = push.inTexture.SampleLevel(push.sampler, uv + float2(0, 0), 0).rgb;
    float3 f = push.inTexture.SampleLevel(push.sampler, uv + float2(x, 0), 0).rgb;

    float3 g = push.inTexture.SampleLevel(push.sampler, uv + float2(-x, -y), 0).rgb;
    float3 h = push.inTexture.SampleLevel(push.sampler, uv + float2(0, -y), 0).rgb;
    float3 i = push.inTexture.SampleLevel(push.sampler, uv + float2(x, -y), 0).rgb;

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