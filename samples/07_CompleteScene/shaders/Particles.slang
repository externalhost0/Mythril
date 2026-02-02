import GPUStructs;

struct VSInput {
    uint VertexID : SV_VertexID;
    uint InstanceID : SV_InstanceID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    float4 ClipPos : SV_Position;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float3 WorldPosition : POSITION;
    float3 color;
};

ParameterBlock<FrameData> frame;

struct ParticleData {
    float3 position;
    float size;
    float3 velocity;
    float3 color;
}
struct QuadVertex {
    float2 pos;
    float2 uv;
}
struct PushConstant {
    Ptr<QuadVertex> vba;
    Ptr<ParticleData> particles;
    float brightness;
    float speed;
}

[[vk::push_constant]]
PushConstant push;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    QuadVertex vertex = push.vba[input.VertexID];
    ParticleData particle = push.particles[input.InstanceID];

    float3 camRight = frame.camera.getRight();
    float3 camUp = frame.camera.getUp();
    float3 camForward = frame.camera.getForward();


    float3 worldPos =
        particle.position +
        (camRight * vertex.pos.x + camUp * vertex.pos.y) * particle.size;

    output.ClipPos = mul(frame.camera.projView, float4(worldPos, 1));
    output.WorldPosition = worldPos;
    output.UV = vertex.uv;
    // my needs
    output.color = particle.color;
    return output;
} 

// [shader("fragment")]
// FSOutput fs_main(v2f input) {
//     FSOutput output;

//     output.FragColor = float4(input.color * push.brightness, 0.25f);
//     return output;
// }

#define kNumLights 4
[shader("fragment")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    // base particle color
    float3 particleColor = input.color * 0.75;
    float lightBrightness = 0.0;
    [unroll(kNumLights)]
    for (int i = 0; i < kNumLights; ++i) {
       float3 lightPos = frame.lighting.pointLights[i].position;
        float lightRange = frame.lighting.pointLights[i].range;
        float lightIntensity = frame.lighting.pointLights[i].intensity;
        float3 lightColor = frame.lighting.pointLights[i].color;
        float3 lightDirection = lightPos - input.WorldPosition;
        float distance = length(lightDirection);
        float attenuation = clamp(1.0 - (distance * distance) / (lightRange * lightRange), 0.0, 1.0);
        float3 radiance = lightColor * attenuation * (lightIntensity / distance);
        float contribution = length(radiance); 
        lightBrightness += contribution;
        particleColor += radiance * 0.5; 
    }

    float3 litColor = particleColor * (1.0 + lightBrightness);

    output.FragColor = float4(litColor, 0.25f);
    return output;
}