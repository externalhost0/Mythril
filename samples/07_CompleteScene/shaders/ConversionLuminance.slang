import GPUStructs;

struct PushConstants
{
    DescriptorHandle<Texture2D> inTex;
    DescriptorHandle<RWTexture2D> outTex;
    DescriptorHandle<SamplerState> sampler;
}
[vk::push_constant]
PushConstants push;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 threadId: SV_DispatchThreadID)
{
    int2 size;
    push.inTex.GetDimensions(size.x, size.y);

    float sumLum = 0.0;

    for (int y = 0; y < size.y; y++)
    {
        for (int x = 0; x < size.x; x++)
        {
            float3 color = push.inTex.SampleLevel(push.sampler, (float2(x, y) + 0.5)/size, 0).rgb;
            float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
            sumLum += lum;
        }
    }
    float avgLum = (sumLum / float(size.x * size.y)) * 3.3; // hardcoded value for bright exposure adjustment
    push.outTex.Store(int2(0, 0), float4(avgLum, avgLum, avgLum, 1.0));
}