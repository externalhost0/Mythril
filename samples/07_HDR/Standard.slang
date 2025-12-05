#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    float4 ClipPos : SV_Position;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float3 WorldPosition : POSITION;
    float4 Tangent : TANGENT;
};

[[vk::push_constant]]
GeometryPushConstants push;

ParameterBlock<FrameData> frame;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = push.vba[input.VertexID];

    float3 worldpos = mul(push.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(mul(frame.camera.proj, frame.camera.view), float4(worldpos, 1));

    output.Normal = mul(push.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    return output;
}

float linearizeDepth(float vertexZ, CameraData camera) {
    float ndc = vertexZ; // already in [-1,1]
    float linearDepth = (2.0 * camera.near * camera.far) / (camera.far + camera.near - ndc * (camera.far - camera.near));
    return linearDepth / camera.far;
}
float3 TransformTangentNormal(float3 n, float3 T, float3 B, float3 N) {
    float3x3 TBN = float3x3(T, B, N);
    return normalize(mul(n, TBN));
}
float3 ComputeBitangent(float3 normal, float4 tangent) {
    float3 T = normalize(tangent.xyz);
    float3 N = normalize(normal);
    float handedness = tangent.w;
    float3 B = normalize(cross(N, T) * handedness);
    return B;
}

static constexpr float PI = 3.14159f;

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    // ----------------------------------------------------
    // Sample base color (albedo)
    // ----------------------------------------------------
    float4 baseColor = push.baseColorTexture.Sample(push.samplerState, input.UV);
    float3 albedo = baseColor.rgb * push.tintColor.rgb;
    float alpha = baseColor.a;

    // ----------------------------------------------------
    // Reconstruct normal from normal map
    // ----------------------------------------------------
    float3 N = normalize(input.Normal);
    float3 T = normalize(input.Tangent.xyz);
    float3 B = ComputeBitangent(N, input.Tangent);

    float3x3 TBN = float3x3(T, B, N);

    float3 nMap = push.normalTexture.Sample(push.samplerState, input.UV).xyz * 2.0 - 1.0;
    float3 Nworld = normalize(mul(nMap, TBN));

    // ----------------------------------------------------
    // Roughness + Metallic
    // glTF convention: G = roughness, B = metallic
    // ----------------------------------------------------
    float3 rm = push.roughnessMetallicTexture.Sample(push.samplerState, input.UV).rgb;
    float roughness = rm.g;
    float metallic = rm.b;

    // ----------------------------------------------------
    // Light + view
    // ----------------------------------------------------
    float3 lightDirection = float3(0.2, -1.0, 1.3); // "sun" direction
    // light color * intensity
    float3 lightColor = float3(1, 1, 0.9) * 2.f;

    float3 L = normalize(-lightDirection); // directional light
    float3 V = normalize(frame.camera.position - input.WorldPosition);
    float3 H = normalize(L + V);


    float NoL = saturate(dot(Nworld, L));
    float NoV = saturate(dot(Nworld, V));
    float NoH = saturate(dot(Nworld, H));
    float VoH = saturate(dot(V, H));

    // ----------------------------------------------------
    // Fresnel (Schlick)
    // ----------------------------------------------------
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);

    // ----------------------------------------------------
    // GGX Normal Distribution
    // ----------------------------------------------------
    float a = roughness * roughness;
    float a2 = a * a;

    float denom = (NoH * NoH) * (a2 - 1.0) + 1.0;
    float D = a2 / max(PI * denom * denom, 0.0001);

    // ----------------------------------------------------
    // Smith Visibility (Schlick-GGX)
    // ----------------------------------------------------
    float k = (roughness + 1.0);
    k = (k * k) * 0.125;

    float Gv = NoV / (NoV * (1.0 - k) + k);
    float Gl = NoL / (NoL * (1.0 - k) + k);
    float Gs = Gv * Gl;

    // ----------------------------------------------------
    // Specular + diffuse
    // ----------------------------------------------------
    float3 specular = (D * Gs * F) / max(4.0 * NoV * NoL, 0.001);

    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo * (1.0 / PI);

    float3 color = (diffuse + specular) * lightColor * NoL;

    output.FragColor = float4(color, alpha);
    return output;
}