
// REQUIRED FOR SLANG
export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handleValue) {
    return defaultGetDescriptorFromHandle(handleValue, BindlessDescriptorOptions.None);
}

struct Vertex
{
    float3 position;
    float uv_x;
    float3 normal;
    float uv_y;
    float4 tangent;
};

static constexpr float kPI = 3.141592f;
static constexpr float k2PI = 6.283185f;

struct PBRBRDF
{
    float3 albedo;
    float roughness;
    float metallic;

    float3 evaluate(float3 V, float3 L, float3 N)
    {
        float3 H = normalize(L + V);

        float NoL = saturate(dot(N, L));
        float NoV = saturate(dot(N, V));
        float NoH = saturate(dot(N, H));
        float VoH = saturate(dot(V, H));

        // Fresnel (Schlick)
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        float3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);

        // GGX Distribution
        float a = roughness * roughness;
        float a2 = a * a;
        float denom = (NoH * NoH) * (a2 - 1.0) + 1.0;
        float D = a2 / max(kPI * denom * denom, 0.0001);

        // Smith Visibility (Schlick-GGX)
        float k = (roughness + 1.0);
        k = (k * k) * 0.125;
        float Gv = NoV / max(NoV * (1.0 - k) + k, 0.001);
        float Gl = NoL / max(NoL * (1.0 - k) + k, 0.001);
        float Gs = Gv * Gl;

        // Cook-Torrance BRDF
        float3 specular = (D * Gs * F) / max(4.0 * NoV * NoL, 0.001);
        float3 kd = (1.0 - F) * (1.0 - metallic);
        float3 diffuse = kd * albedo / kPI;

        return (diffuse + specular) * NoL;
    }
};

struct SurfaceGeometry
{
    float3 position;
    float3 normal;
}

// general interface that all lights must be able to illuminate
interface ILight
{
    float3 illuminate(float3 worldPos, float3 normal, PBRBRDF brdf, float3 viewDir);
}

struct DirectionalLight : ILight
{
    float3 color;
    float intensity;
    float3 direction;

    float3 illuminate(float3 worldPos, float3 normal, PBRBRDF brdf, float3 viewDir)
    {
        float3 L = normalize(-direction);
        float3 N = normalize(normal);

        float3 radiance = color * intensity;
        return brdf.evaluate(viewDir, L, N) * radiance;
    }
}

struct CascadedShadowMap
{
    DescriptorHandle<Texture2D> shadowTextures[4];
    SamplerComparisonState shadowSampler;
    float4 shadowDistances;
}
// implements directional light alongside a shadowmap
struct SkyLight
{
    DirectionalLight directionalLight;
    CascadedShadowMap cascadedMap;
}

struct PointLight : ILight
{
    float3 color;
    float intensity;
    float3 position;
    float range;

    float3 illuminate(float3 worldPos, float3 normal, PBRBRDF brdf, float3 viewDir)
    {
        float3 lightDirection = position - worldPos;
        float distance = length(lightDirection);
        float3 direction = normalize(lightDirection);
        float3 N = normalize(normal);
        float attenuation = clamp(1.0 - (distance * distance) / (range * range), 0.0, 1.0);
        float3 radiance = color * attenuation * (intensity / distance);

        return brdf.evaluate(viewDir, direction, N) * radiance;
    }
}

struct EnvironmentLight : ILight
{
    float3 color;
    float intensity;

    float3 illuminate(float3 worldPos, float3 normal, PBRBRDF brdf, float3 viewDir)
    {
        // Simple ambient - modified by material's diffuse response
        // Metals receive less ambient contribution
        float3 kd = lerp(float3(1.0, 1.0, 1.0), float3(0.0, 0.0, 0.0), brdf.metallic);
        return brdf.albedo * kd * color * intensity;
    }
}

struct LightingData
{
    PointLight pointLights[4];
    DirectionalLight directionalLight;
    EnvironmentLight environmentLight;

    float3 illuminate(float3 worldPos, float3 normal, PBRBRDF brdf, float3 viewDir)
    {
        float3 result = 0;

        for (int i = 0; i < 4; ++i)
        {
            result += pointLights[i].illuminate(worldPos, normal, brdf, viewDir);
        }
        result += environmentLight.illuminate(worldPos, normal, brdf, viewDir);
        result += directionalLight.illuminate(worldPos, normal, brdf, viewDir);

        return result;
    }
};

struct CameraData
{
    float4x4 proj;
    float4x4 view;
    float3 position;
    float far;
    float near;

    float3 getRight() {
        // Row 0 of view matrix
        return normalize(float3(
            view._11,
            view._12,
            view._13
        ));
    }

    float3 getUp() {
        // Row 1 of view matrix
        return normalize(float3(
            view._21,
            view._22,
            view._23
        ));
    }

    float3 getForward() {
        // Camera looks down -Z
        return normalize(-float3(
            view._31,
            view._32,
            view._33
        ));
    }
};

struct FrameData
{
    CameraData camera;
    LightingData lighting;
    float time;
    int one;
};

struct GeometryPushConstants
{
    float4x4 model;
    Ptr<Vertex> vba;
    float4 tintColor;
    DescriptorHandle<Texture2D> baseColorTexture;
    DescriptorHandle<Texture2D> normalTexture;
    DescriptorHandle<Texture2D> roughnessMetallicTexture;
    DescriptorHandle<SamplerState> linearSampler;
    DescriptorHandle<Texture2D> shadowTexture;
    DescriptorHandle<SamplerState> shadowSampler;
    float4x4 lightSpaceMatrix;
    float minBias;
    float maxBias;
};
