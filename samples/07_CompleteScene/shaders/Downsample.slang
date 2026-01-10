import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D<float3>> inTexture;
    DescriptorHandle<RWTexture2D<float3>> outTexture;
    DescriptorHandle<SamplerState> sampler;
}
[vk::push_constant]
PushConstants push;


[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 threadId: SV_DispatchThreadID) {
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
    float2 texelSize = 1.0 / inSize;

    float x = texelSize.x;
    float y = texelSize.y;

    // Sample using hardware texture filtering
    float3 a = push.inTexture.SampleLevel(push.sampler, uv + float2(-2 * x, 2 * y), 0);
    float3 b = push.inTexture.SampleLevel(push.sampler, uv + float2(0, 2 * y), 0);
    float3 c = push.inTexture.SampleLevel(push.sampler, uv + float2(2 * x, 2 * y), 0);

    float3 d = push.inTexture.SampleLevel(push.sampler, uv + float2(-2 * x, 0), 0);
    float3 e = push.inTexture.SampleLevel(push.sampler, uv + float2(0, 0), 0);
    float3 f = push.inTexture.SampleLevel(push.sampler, uv + float2(2 * x, 0), 0);

    float3 g = push.inTexture.SampleLevel(push.sampler, uv + float2(-2 * x, -2 * y), 0);
    float3 h = push.inTexture.SampleLevel(push.sampler, uv + float2(0, -2 * y), 0);
    float3 i = push.inTexture.SampleLevel(push.sampler, uv + float2(2 * x, -2 * y), 0);

    float3 j = push.inTexture.SampleLevel(push.sampler, uv + float2(-x, y), 0);
    float3 k = push.inTexture.SampleLevel(push.sampler, uv + float2(x, y), 0);
    float3 l = push.inTexture.SampleLevel(push.sampler, uv + float2(-x, -y), 0);
    float3 m = push.inTexture.SampleLevel(push.sampler, uv + float2(x, -y), 0);

    // Apply weighted distribution
    float3 result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.125;

    push.outTexture[outputCoord] = result;
}