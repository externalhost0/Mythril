import Scene;

struct BoundsPushConstant {
    Ptr<Instance> instances;
    Ptr<Geometry> geometries;
    Ptr<Bounds> bounds;
    uint instanceCount;
    uint padding;
}

[[vk::push_constant]]
BoundsPushConstant push;

static const uint kGroupSize = 64;
groupshared float3 sharedLo[kGroupSize];
groupshared float3 sharedHi[kGroupSize];

[shader("compute")]
[numthreads(kGroupSize, 1, 1)]
void cs_main(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    uint instanceIndex = group.x;
    Instance instance = push.instances[instanceIndex];
    uint geometryIndex = instance.customIndexAndMask & 0xFFFFFFu;
    Geometry geometry = push.geometries[geometryIndex];
    float3 lo = float3(1e30);
    float3 hi = float3(-1e30);
    for (uint v = thread.x; v < geometry.vertexCount; v += kGroupSize) {
        float4 p = float4(geometry.vertices[v].position, 1.0);
        float3 world = float3(dot(instance.row0, p), dot(instance.row1, p), dot(instance.row2, p));
        lo = min(lo, world);
        hi = max(hi, world);
    }
    sharedLo[thread.x] = lo;
    sharedHi[thread.x] = hi;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kGroupSize / 2; stride > 0; stride /= 2) {
        if (thread.x < stride) {
            sharedLo[thread.x] = min(sharedLo[thread.x], sharedLo[thread.x + stride]);
            sharedHi[thread.x] = max(sharedHi[thread.x], sharedHi[thread.x + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread.x == 0)
        push.bounds[instanceIndex] = Bounds(sharedLo[0], geometryIndex, sharedHi[0], 0.0);
}
