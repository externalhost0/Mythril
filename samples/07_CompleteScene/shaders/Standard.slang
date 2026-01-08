import GPUStructs;

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
    float4 LightSpacePos;
};

[[vk::push_constant]]
GeometryPushConstants push;

ParameterBlock<FrameData> frame;

// for transforming the shadow map correctly
// same as: projCoords.xy = projCoords.xy * 0.5 + 0.5;
static const float4x4 biasMat = float4x4(
    0.5, 0.0, 0.0, 0.5,
    0.0, 0.5, 0.0, 0.5,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0);

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = push.vba[input.VertexID];

    float3 worldpos = mul(push.model, float4(v.position, 1)).xyz;
    output.ClipPos = mul(mul(frame.camera.proj, frame.camera.view), float4(worldpos, 1));

    output.Normal = mul(push.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    output.LightSpacePos = mul(biasMat, mul(push.lightSpaceMatrix, mul(push.model, float4(v.position, 1))));

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

[shader("fragment")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float4 sampledColor = push.baseColorTexture.Sample(push.linearSampler, input.UV);
    float3 albedo = sampledColor.rgb * push.tintColor.rgb;
    float alpha = sampledColor.a;

    // Reconstruct normal from normal map
    float3 N = normalize(input.Normal);
    float3 T = normalize(input.Tangent.xyz);
    float3 B = ComputeBitangent(N, input.Tangent);
    float3x3 TBN = float3x3(T, B, N);
    float3 nMap = push.normalTexture.Sample(push.linearSampler, input.UV).xyz * 2.0 - 1.0;
    float3 Nworld = normalize(mul(nMap, TBN));

    // Sample roughness + metallic
    float3 rm = push.roughnessMetallicTexture.Sample(push.linearSampler, input.UV).rgb;
    float roughness = clamp(rm.g, 0.04, 1.0); // Clamp to avoid artifacts
    float metallic = rm.b;

    // Prepare BRDF with material properties
    PBRBRDF brdf;
    brdf.albedo = albedo;
    brdf.roughness = roughness;
    brdf.metallic = metallic;

    float3 viewDir = normalize(frame.camera.position - input.WorldPosition);

    float3 ambient = frame.lighting.environmentLight.illuminate(input.WorldPosition, Nworld, brdf, viewDir);
    float3 direct = frame.lighting.directionalLight.illuminate(input.WorldPosition, Nworld, brdf, viewDir);
    float3 omni = 0.0;
    for (int i = 0; i < 4; ++i) {
        omni += frame.lighting.pointLights[i].illuminate(input.WorldPosition, Nworld, brdf, viewDir);
    }

    float shadowFactor = ShadowCalculation(input.LightSpacePos);
    float3 finalColor = ambient + omni + (direct * (1-shadowFactor));

    output.FragColor = float4(finalColor, alpha);
    return output;
}

// BROKEN in slang!
// float ShadowCalculationHardware(float4 lightspacePos) {
//     float3 projCoords = lightspacePos.xyz / lightspacePos.w;
//     if (projCoords.z > 1.0)
//         return 0.0;
//     projCoords.y = 1.0 - projCoords.y;
//     // bounds check
//     if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
//         projCoords.y < 0.0 || projCoords.y > 1.0)
//         return 1.0;
//     return push.shadowTexture.SampleCmpLevelZero(push.shadowSampler, projCoords.xy, projCoords.z);
// }

float ShadowCalculation(float4 lightspacePos) {
    float3 projCoords = lightspacePos.xyz / lightspacePos.w;
    if (projCoords.z > 1.0)
        return 0.0;
    projCoords.y = 1.0 - projCoords.y;
    // bounds check
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;
    float currentDepth = projCoords.z;

    // PCF 3x3
    int2 texDim;
    push.shadowTexture.GetDimensions(texDim.x, texDim.y);
    float scale = 1.5;
    float2 texelSize = scale * 1.0 / texDim;

    float shadow = 0.0;
    int count = 0;
    int range = 1;
    for (int x = -range; x <= range; x++) {
        for (int y = -range; y <= range; y++) {
            float pcfDepth = push.shadowTexture.Sample(push.shadowSampler, projCoords.xy + float2(x, y) * texelSize).r;
            shadow += currentDepth > pcfDepth ? 1.0 : 0.0;
            count++;
        }
    }
    return shadow / count;
}

// float ShadowCalculationEXT(float4 lightspacePos, float bias) {
//     float3 projCoords = lightspacePos.xyz / lightspacePos.w;
//     if (projCoords.z > 1.0)
//         return 0.0;
//     projCoords.xy = projCoords.xy * 0.5 + 0.5;
//     projCoords.y = 1.0 - projCoords.y;
//     // get closest depth value from light's perspective (using [0,1] range fragPosLight as coords)
//     float closestDepth = push.shadowTexture.Sample(push.shadowSampler, projCoords.xy).r;
//     // get depth of current fragment from light's perspective
//     float currentDepth = projCoords.z;
//     // check whether current frag pos is in shadow
//     float shadow = 0.0;

//     // PCF 3x3
//     int2 texDim;
//     push.shadowTexture.GetDimensions(texDim.x, texDim.y);
//     float scale = 1.5;
//     float2 texelSize = scale * 1.0 / texDim;

//     int count = 0;
//     int range = 1;
//     for (int x = -range; x <= range; x++) {
//         for (int y = -range; y <= range; y++) {
//             float pcfDepth = push.shadowTexture.Sample(push.shadowSampler, projCoords.xy + float2(x, y) * texelSize).r;
//             shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
//             count++;
//         }
//     } 
//     return shadow / count;
// }