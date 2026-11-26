import Scene;

struct BoundsLinesPushConstant {
    float4x4 viewProjection;
    float3 cameraPosition;
    uint boundsCount;
    Ptr<Bounds> bounds;
    DescriptorHandle<Texture2D<float>> sceneDistance;
    float2 viewportSize;
    float halfWidth;
    float padding;
}

[[vk::push_constant]]
BoundsLinesPushConstant push;

static const uint kVerticesPerEdge = 6;
static const uint kEdgesPerBox = 12;
static const uint kVerticesPerBox = kVerticesPerEdge * kEdgesPerBox;

static const uint2 kEdges[kEdgesPerBox] = {
    uint2(0, 1), uint2(2, 3), uint2(4, 5), uint2(6, 7),
    uint2(0, 2), uint2(1, 3), uint2(4, 6), uint2(5, 7),
    uint2(0, 4), uint2(1, 5), uint2(2, 6), uint2(3, 7),
};
struct QuadCorner {
    bool atEnd;
    float side;
};
static const QuadCorner kQuad[kVerticesPerEdge] = {
    { false, -1.0 }, { true, -1.0 }, { true, 1.0 },
    { false, -1.0 }, { true, 1.0 }, { false, 1.0 },
};

struct v2f {
    float4 position : SV_Position;
    float3 world : WORLD;
    nointerpolation float3 color : COLOR;
};

float3 cornerOf(float3 lo, float3 hi, uint corner) {
    return float3((corner & 1u) != 0u ? hi.x : lo.x, (corner & 2u) != 0u ? hi.y : lo.y, (corner & 4u) != 0u ? hi.z : lo.z);
}

float3 boundsColor(uint geometry) {
    if (geometry == kGeometryGround)
        return float3(0.3, 0.6, 1.0);
    return geometry == kGeometryLeaf ? float3(1.0, 0.85, 0.2) : float3(0.3, 1.0, 0.4);
}

[shader("vertex")]
v2f vs_main(uint vertexId : SV_VertexID) {
    uint box = vertexId / kVerticesPerBox;
    uint2 edge = kEdges[(vertexId / kVerticesPerEdge) % kEdgesPerBox];
    QuadCorner corner = kQuad[vertexId % kVerticesPerEdge];

    float3 lo, hi, color;
    bool instanceBox = box < push.boundsCount;
    if (instanceBox) {
        Bounds bounds = push.bounds[box];
        lo = bounds.lo;
        hi = bounds.hi;
        color = boundsColor(bounds.geometry);
    } else {
        lo = float3(1e30);
        hi = float3(-1e30);
        for (uint i = 0; i < push.boundsCount; i++) {
            lo = min(lo, push.bounds[i].lo);
            hi = max(hi, push.bounds[i].hi);
        }
        color = float3(1.0);
    }

    float3 worldA = cornerOf(lo, hi, edge.x);
    float3 worldB = cornerOf(lo, hi, edge.y);
    float4 clipA = mul(push.viewProjection, float4(worldA, 1.0));
    float4 clipB = mul(push.viewProjection, float4(worldB, 1.0));
    v2f output;
    output.color = color;
    // Vulkan clips at z = 0, so pull an endpoint behind the camera onto that plane before the divide
    if (clipA.z < 0.0 && clipB.z < 0.0) {
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        output.world = worldA;
        return output;
    }
    if (clipA.z < 0.0 || clipB.z < 0.0) {
        float t = clipA.z / (clipA.z - clipB.z);
        if (clipA.z < 0.0) {
            clipA = lerp(clipA, clipB, t);
            worldA = lerp(worldA, worldB, t);
        } else {
            clipB = lerp(clipA, clipB, t);
            worldB = lerp(worldA, worldB, t);
        }
    }

    float2 pixelA = clipA.xy / clipA.w * push.viewportSize;
    float2 pixelB = clipB.xy / clipB.w * push.viewportSize;
    float2 along = pixelB - pixelA;
    along = dot(along, along) > 1e-8 ? normalize(along) : float2(1.0, 0.0);
    float2 offset = float2(-along.y, along.x) * corner.side * push.halfWidth * 2.0 / push.viewportSize;

    float4 clip = corner.atEnd ? clipB : clipA;
    output.position = float4(clip.xy + offset * clip.w, clip.zw);
    output.world = corner.atEnd ? worldB : worldA;
    return output;
}

[shader("pixel")]
float4 fs_main(v2f input) : SV_Target0 {
    float sceneDistance = push.sceneDistance.Load(int3(int2(input.position.xy), 0));
    bool visible = length(input.world - push.cameraPosition) <= sceneDistance * 1.001;
    return float4(input.color, visible ? 1.0 : 0.35);
}
