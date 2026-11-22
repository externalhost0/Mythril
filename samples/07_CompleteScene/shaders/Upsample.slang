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

    uint2 inSize;
    push.inTexture.GetDimensions(inSize.x, inSize.y);
    // 4 bilinear taps at half the radius equal the 3x3 tent, exactly so at a radius of 1 texel
    float2 o = 0.5 * push.filterRadius / float2(inSize);

    float3 result = push.inTexture.SampleLevel(push.sampler, uv + float2(-o.x, -o.y), 0).rgb;
    result += push.inTexture.SampleLevel(push.sampler, uv + float2(o.x, -o.y), 0).rgb;
    result += push.inTexture.SampleLevel(push.sampler, uv + float2(-o.x, o.y), 0).rgb;
    result += push.inTexture.SampleLevel(push.sampler, uv + o, 0).rgb;
    result *= 0.25;

    float3 prev = push.outTexture[outputCoord].rgb;
    push.outTexture[outputCoord] = float4(prev + result, 1);
}
