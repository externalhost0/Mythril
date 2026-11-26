import Scene;

struct WindPushConstant {
    Ptr<Vertex> rest;
    Ptr<Vertex> bent;
    float3 root;
    float height;
    float2 windDirection;
    float time;
    float strength;
    uint vertexCount;
    float phase;
}

[[vk::push_constant]]
WindPushConstant push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 id: SV_DispatchThreadID) {
    if (id.x >= push.vertexCount)
        return;
    float3 p = push.rest[id.x].position;
    float h = saturate((p.y - push.root.y) / push.height);
    float gust = sin(push.time * 1.3 + push.phase) + 0.45 * sin(push.time * 3.1 + push.phase * 2.0);
    float flutter = sin(push.time * 9.0 + dot(p, float3(3.1, 1.7, 2.3))) * h * h * h * 0.12;
    float sway = push.strength * (h * h * gust + flutter);
    float2 offset = push.windDirection * sway;
    float drop = 0.5 * sway * sway / max(push.height, 1e-3);
    push.bent[id.x].position = float3(p.x + offset.x, p.y - drop, p.z + offset.y);
}
