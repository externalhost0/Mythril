#include "GPUStructs.h"

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    float4 ClipPos : SV_Position;
    float3 Normal  : NORMAL;
    float3 Color   : COLOR0;
};

[vk::push_constant]
SkinPushConstant push;

float3 quatRotate(float4 q, float3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float3 heatmap(float t) {
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

float3 jointColor(uint joint) {
    float h = frac(float(joint) * 0.618034);
    return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

void skinLinear(SkinnedVertex v, inout float3 pos, inout float3 nrm) {
    float4x4 skin = v.weights.x * push.jointMatrices[v.joints.x]
                  + v.weights.y * push.jointMatrices[v.joints.y]
                  + v.weights.z * push.jointMatrices[v.joints.z]
                  + v.weights.w * push.jointMatrices[v.joints.w];
    pos = mul(skin, float4(pos, 1.0)).xyz;
    nrm = mul((float3x3)skin, nrm);
}

float hemisphereAlignedWeight(float weight, float4 rotation, float4 pivot) {
    return dot(rotation, pivot) < 0.0 ? -weight : weight;
}

void skinDualQuat(SkinnedVertex v, inout float3 pos, inout float3 nrm) {
    float4 pivot = push.jointDualQuats[v.joints.x].real;
    float4 real = 0.0;
    float4 dual = 0.0;
    for (uint i = 0; i < 4; i++) {
        DualQuat dq = push.jointDualQuats[v.joints[i]];
        float w = hemisphereAlignedWeight(v.weights[i], dq.real, pivot);
        real += w * dq.real;
        dual += w * dq.dual;
    }
    float len = length(real);
    real /= len;
    dual /= len;
    float3 translation = 2.0 * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));
    pos = quatRotate(real, pos) + translation;
    nrm = quatRotate(real, nrm);
}

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;
    SkinnedVertex v = push.vertices[input.VertexID];

    float3 pos = v.position;
    float3 nrm = v.normal;
    if (push.skinningMode == kSkinLinear)
        skinLinear(v, pos, nrm);
    else if (push.skinningMode == kSkinDualQuat)
        skinDualQuat(v, pos, nrm);

    float3 worldPos = mul(push.model, float4(pos, 1.0)).xyz;
    output.ClipPos = mul(push.frame.viewProj, float4(worldPos, 1.0));
    output.Normal = mul((float3x3)push.model, nrm);

    float3 color = float3(0.8, 0.8, 0.8);
    if (push.viewMode == kViewWeights) {
        float weight = 0.0;
        for (uint i = 0; i < 4; i++)
            weight += v.joints[i] == push.selectedJoint ? v.weights[i] : 0.0;
        color = heatmap(weight);
    } else if (push.viewMode == kViewInfluence) {
        color = v.weights.x * jointColor(v.joints.x)
              + v.weights.y * jointColor(v.joints.y)
              + v.weights.z * jointColor(v.joints.z)
              + v.weights.w * jointColor(v.joints.w);
    }
    output.Color = color;
    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float3 n = normalize(input.Normal);
    float ndl = saturate(dot(n, normalize(push.frame.lightDir)));
    float shade = push.viewMode == kViewLit ? 0.15 + 0.85 * ndl : 0.45 + 0.55 * ndl;
    output.FragColor = float4(input.Color * shade, 1.0);
    return output;
}
