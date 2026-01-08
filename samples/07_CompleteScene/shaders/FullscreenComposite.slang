struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
}

static const int ToneMappingMode_None = 0;
static const int ToneMappingMode_Reinhard = 1;
static const int ToneMappingMode_Uchimura = 2;
static const int ToneMappingMode_KhronosPBR = 3;

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

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float3 color = push.colorTexture.Sample(push.sampler, input.UV).rgb;
    float3 bloom = push.bloomTexture.Sample(push.sampler, input.UV).rgb;
    float avgLuminance = push.avgLumTexture.SampleLevel(push.sampler, float2(0.5), 10).r;

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

    output.FragColor = float4(color + (push.bloomStrength * bloom), 1);
    return output;
}