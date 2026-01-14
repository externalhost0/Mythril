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
    float3 normal;
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

    output.ClipPos = mul(frame.camera.proj, mul(frame.camera.view, float4(worldPos, 1)));

    output.WorldPosition = worldPos;

    output.Normal = vertex.normal;
    output.UV = vertex.uv;

    // my needs
    output.color = particle.color;


    return output;
} 

[shader("fragment")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    output.FragColor = float4(input.color * push.brightness, 0.25f);
    return output;
}