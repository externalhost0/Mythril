struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
}

static constexpr int ToneMappingMode_None = 0;
static constexpr int ToneMappingMode_Reinhard = 1;
static constexpr int ToneMappingMode_Uchimura = 2;
static constexpr int ToneMappingMode_KhronosPBR = 3;
static constexpr int ToneMappingMode_AgX = 4;

struct FullscreenPushConstant {
    DescriptorHandle<Texture2D> colorTexture;
    DescriptorHandle<Texture2D> avgLumTexture;
    DescriptorHandle<Texture2D> bloomTexture;
    DescriptorHandle<SamplerState> sampler;
    float exposure;
    float bloomStrength;
    int toneMapMode;
    // implementation dependant

    // Reinhard
    float maxWhite;

    // Uchimura
    float P; // max display brightness
    float a; // contrast
    float m; // linear section start
    float l; // linear section length
    float c; // black tightness
    float b; // pedestal

    // Khronos PBR
    float startCompression; // highlight compression start
    float desaturation;

    // AgX
    float3 slope;
    float3 offset;
    float3 power;
    float saturation;
}

[[vk::push_constant]]
FullscreenPushConstant push;

[shader("vertex")]
v2f vs_main(uint VertexID : SV_VertexID) {
    v2f output;
    // generates the fullscreen triangle without any cpu interaction
    // https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html

    output.UV = float2((VertexID << 1) & 2, VertexID & 2 );
    output.ClipPos = float4(output.UV * 2.0 + -1.0, 0.0, 1.0);
    // flip the y axis
    output.UV.y = 1.0 - output.UV.y;
    return output;
}

// for debugging with perspective camera
float linearizeDepth(float vertexZ, float far, float near) {
    float ndc = vertexZ; // already in [-1,1]
    float linearDepth = (2.0 * near * far) / (far + near - ndc * (far - near));
    return linearDepth / far;
}

// https://github.com/PacktPublishing/3D-Graphics-Rendering-Cookbook-Second-Edition/blob/main/Chapter10/05_HDR/src/ToneMap.frag
float luminance(float3 v) {
    return dot(v, float3(0.2126, 0.7152, 0.0722));
}
float3 uchimura(float3 x, float P, float a, float m, float l, float c, float b) {
    float l0 = ((P - m) * l) / a;
    float L0 = m - m / a;
    float L1 = m + (1.0 - m) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    float3 w0 = float3(1.0 - smoothstep(0.0, m, x));
    float3 w2 = float3(step(m + l0, x));
    float3 w1 = float3(1.0 - w0 - w2);

    float3 T = float3(m * pow(x / m, float3(c)) + b);
    float3 S = float3(P - (P - S1) * exp(CP * (x - S0)));
    float3 L = float3(m + a * (x - m));

    return T * w0 + L * w1 + S * w2;
}
float3 reinhard2(float3 v, float maxWhite) {
    float l_old = luminance(v);
    float l_new = l_old * (1.0 + (l_old / (maxWhite * maxWhite))) / (1.0 + l_old);
    return v * (l_new / l_old);
}
// Khronos PBR Neutral Tone Mapper:
// https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md#pbr-neutral-specification
// https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/pbrNeutral.glsl
float3 PBRNeutralToneMapping(float3 color, float startCompression, float desaturation) {
    startCompression -= 0.04;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1. - startCompression;
    float newPeak = 1. - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
    return lerp(color, newPeak * float3(1, 1, 1), g);
}
// and our own implementation of AgX
static constexpr float3x3 LINEAR_REC2020_TO_LINEAR_SRGB = float3x3(
    1.6605, -0.1246, -0.0182,
    -0.5876, 1.1329, -0.1006,
    -0.0728, -0.0083, 1.1187
);

static constexpr float3x3 LINEAR_SRGB_TO_LINEAR_REC2020 = float3x3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0113, 0.8956
);

// Converted to column major from blender: https://github.com/blender/blender/blob/fc08f7491e7eba994d86b610e5ec757f9c62ac81/release/datafiles/colormanagement/config.ocio#L358
static constexpr float3x3 AGX_INSET_MATRIX = float3x3(
    0.856627153315983, 0.137318972929847, 0.11189821299995,
    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
    0.0482516061458583, 0.101439036467562, 0.811302368396859
);

// Converted to column major and inverted from https://github.com/EaryChow/AgX_LUT_Gen/blob/ab7415eca3cbeb14fd55deb1de6d7b2d699a1bb9/AgXBaseRec2020.py#L25
// https://github.com/google/filament/blob/bac8e58ee7009db4d348875d274daf4dd78a3bd1/filament/src/ToneMapper.cpp#L273-L278
static constexpr float3x3 AGX_OUTSET_MATRIX = float3x3(
    1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
    -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
    -0.016493938717834573, -0.016493938717834257, 1.2519364065950405
);

static constexpr float AGX_MIN_EV = -12.47393;
static constexpr float AGX_MAX_EV = 4.026069;

static constexpr float3 LUMA_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

float3 agxSigmoid(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

float3 agxLook(float3 color, float3 slope, float3 offset, float3 power, float saturation) {
    color = mul(color, LINEAR_SRGB_TO_LINEAR_REC2020);

    color = mul(color, AGX_INSET_MATRIX);

    color = max(color, 1e-10);
    color = clamp(log2(color), AGX_MIN_EV, AGX_MAX_EV);
    color = (color - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV);

    color = agxSigmoid(clamp(color, 0.0, 1.0));

    color = pow(color * slope + offset, power);

    float luma = dot(color, LUMA_WEIGHTS);
    color = luma + saturation * (color - luma);

    color = mul(color, AGX_OUTSET_MATRIX);
    color = pow(max(color, 0.0), 2.2);
    color = mul(color, LINEAR_REC2020_TO_LINEAR_SRGB);
    return clamp(color, 0.0, 1.0);
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float3 color = push.colorTexture.Sample(push.sampler, input.UV).rgb;
    float3 bloom = push.bloomTexture.Sample(push.sampler, input.UV).rgb;
    float avgLuminance = push.avgLumTexture.Sample(push.sampler, float2(0.5)).r;

    int toneMapMode = push.toneMapMode;
    if (toneMapMode != ToneMappingMode_None) {
        float midGray = 0.5;
        color *= push.exposure * midGray / (avgLuminance + 0.001);
    }
    if (toneMapMode == ToneMappingMode_Reinhard) {
        color = reinhard2(push.exposure * color, push.maxWhite);
    }
    if (toneMapMode == ToneMappingMode_Uchimura) {
        color = uchimura(push.exposure * color, push.P, push.a, push.m, push.l, push.c, push.b);
    }
    if (toneMapMode == ToneMappingMode_KhronosPBR) {
        color = PBRNeutralToneMapping(push.exposure * color, push.startCompression, push.desaturation);
    }
    if (toneMapMode == ToneMappingMode_AgX) {
        color = agxLook(push.exposure * color, push.slope, push.offset, push.power, push.saturation);
    }

    output.FragColor = float4(lerp(color, bloom, push.bloomStrength), 1);
    return output;
}