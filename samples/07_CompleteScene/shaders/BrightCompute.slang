import GPUStructs;

struct PushConstants
{
    DescriptorHandle<Texture2D> colorTex;
    DescriptorHandle<RWTexture2D> outTex;     // rgba16
    DescriptorHandle<RWTexture2D> luminanceTex; // r16
    DescriptorHandle<SamplerState> sampler;
    float exposure;
    float pad;
}
[vk::push_constant]
PushConstants push;

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_main(uint3 threadId: SV_DispatchThreadID) {
    int2 sizeIn;
    int2 sizeOut;
    push.colorTex.GetDimensions(sizeIn.x, sizeIn.y);
    push.outTex.GetDimensions(sizeOut.x, sizeOut.y);

    const uint2 xy = threadId.xy;
    const float2 uv0 = (threadId.xy + float2(0)) / sizeOut;
    const float2 uv1 = (threadId.xy + float2(1)) / sizeOut;

    if (xy.x > sizeIn.x || xy.y > sizeIn.y)
        return;

    float2 dxdy = (uv1 - uv0) / 3;

    float3 color = float3(0.f);

    // 3x3 box filter
    for (int v = 0; v != 3; v++) {
        for (int u = 0; u != 3; u++) {
            color += push.colorTex.SampleLevel(push.sampler, uv0 + float2(u, v) * dxdy, 0).rgb;
        }
    }

    float luminance = push.exposure * dot(color.rgb / 9, float3(0.2126, 0.7152, 0.0722));

    float3 rgb = luminance > 1.f ? color.rgb : float3(0.f);

    push.outTex.Store(xy, float4(rgb, 1.f));
    push.luminanceTex.Store(xy, float4(luminance));
}
