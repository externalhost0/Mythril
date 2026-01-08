import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D> inTex;
    DescriptorHandle<RWTexture2D> outTex;
    DescriptorHandle<SamplerState> sampler;
}

[vk::push_constant]
PushConstants push;

ParameterBlock<FrameData> frame;

static const int kFilterSize = 17;
// https://drdesten.github.io/web/tools/gaussian_kernel/
static const float kGaussWeights[kFilterSize] = float[](
    0.00001525878906,
    0.0002441406250,
    0.001831054688,
    0.008544921875,
    0.02777099609,
    0.06665039063,
    0.1221923828,
    0.1745605469,
    0.1963806152,
    0.1745605469,
    0.1221923828,
    0.06665039063,
    0.02777099609,
    0.008544921875,
    0.001831054688,
    0.0002441406250,
    0.00001525878906
);

[SpecializationConstant]
const bool kIsHorizontal;

[shader("compute")]
[numthreads(16, 16)]
void cs_main(uint3 threadId: SV_DispatchThreadID) {
    int2 size;
    push.inTex.GetDimensions(size.x, size.y);
    const int2 xy = threadId.xy;

    if (xy.x > size.x || xy.y > size.y)
        return;

    const float2 texCoord = (threadId.xy + float2(0.5)) / size;
    const float texScaler = 1.0 * frame.one  / (kIsHorizontal ? size.x : size.y);

    float3 c = float3(0.0);

    for (int i = 0; i != kFilterSize; i++) {
        float offset = float(i - kFilterSize / 2);
        float2 uv = texCoord + texScaler * (kIsHorizontal ? float2(offset, 0) : float2(0, offset));
        c += push.inTex.SampleLevel(push.sampler, uv, 0).rgb * kGaussWeights[i];
    }

    push.outTex.Store(xy, float4(c, 1));
}