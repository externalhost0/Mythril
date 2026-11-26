import Scene;

struct PerFrame {
    float4x4 viewInverse;
    float4x4 projInverse;
};
struct ShadowPushConstant {
    Ptr<PerFrame> frame;
    float4 lightDir;
    Ptr<Geometry> geometries;
    DescriptorHandle<RWTexture2D<float4>> outTexture;
    DescriptorHandle<RaytracingAccelerationStructure> tlas;
    uint32_t enableShadows;
    uint32_t debugMode;
    DescriptorHandle<RWTexture2D<float>> boundsHitDistance;
};

struct RayPayload {
  float3 color;
  float t;
};
struct ShadowPayload {
  bool isShadowed;
};

[[vk::push_constant]]
ShadowPushConstant push;

Geometry getGeometry() {
    return push.geometries[InstanceID()];
}

RaytracingAccelerationStructure getTLAS() {
    return RaytracingAccelerationStructure(push.tlas);
}

static const uint kMissPrimary = 0;
static const uint kMissShadow = 1;

// must match kDebugModeNames in main.cpp
static const uint kDebugLit = 0;
static const uint kDebugNormals = 1;
static const uint kDebugHitDistance = 2;
static const uint kDebugPrimitiveId = 3;
static const uint kDebugBarycentrics = 4;
static const uint kDebugShadowRays = 5;
static const uint kDebugRayDirection = 6;
static const uint kDebugBounds = 7;

float3 hashColor(uint id) {
    id = (id ^ 61u) ^ (id >> 16);
    id *= 9u;
    id ^= id >> 4;
    id *= 0x27d4eb2du;
    id ^= id >> 15;
    return float3(id & 0xFFu, (id >> 8) & 0xFFu, (id >> 16) & 0xFFu) / 255.0;
}

float3 heatRamp(float t) {
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

[shader("raygeneration")]
void raygenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;

    float2 ndc = (float2(pixel) + 0.5) / float2(size) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float4 origin = mul(push.frame->viewInverse, float4(0, 0, 0, 1));
    float4 target = mul(push.frame->projInverse, float4(ndc, 1, 1));
    float4 direction = mul(push.frame->viewInverse, float4(normalize(target.xyz / target.w), 0));

    RayDesc ray;
    ray.Origin = origin.xyz;
    ray.Direction = normalize(direction.xyz);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    if (push.debugMode == kDebugRayDirection) {
        push.outTexture[pixel] = float4(ray.Direction * 0.5 + 0.5, 1);
        return;
    }

    RayPayload payload;
    payload.color = float3(0.0);
    payload.t = 1e30;
    TraceRay(getTLAS(), RAY_FLAG_NONE, 0xff, 0, 0, kMissPrimary, ray, payload);

    if (push.debugMode == kDebugBounds) {
        payload.color *= 0.6;
        push.boundsHitDistance[pixel] = payload.t;
    }
    push.outTexture[pixel] = float4(payload.color, 1);
}

// primary ray
[shader("miss")]
void missMain(inout RayPayload payload) {
    if (push.debugMode != kDebugLit && push.debugMode != kDebugBounds) {
        payload.color = float3(0.05);
        return;
    }
    float t = 0.5 * (WorldRayDirection().y + 1.0);
    payload.color = lerp(float3(0.9, 0.9, 0.95), float3(0.35, 0.55, 0.9), t);
}
// shadow ray
[shader("miss")]
void missShadowMain(inout ShadowPayload payload) {
    payload.isShadowed = false;
}

float traceShadow(float3 origin, float3 direction) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    ShadowPayload payload;
    payload.isShadowed = true;
    // any hit means occluded, so the miss shader is the only one that needs to run
    const uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(getTLAS(), flags, 0xff, 0, 0, kMissShadow, ray, payload);
    return payload.isShadowed ? 0.0 : 1.0;
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.t = RayTCurrent();
    Geometry geometry = getGeometry();
    uint index = 3 * PrimitiveIndex();
    uint3 tri = uint3(geometry.indices[index + 0], geometry.indices[index + 1], geometry.indices[index + 2]);
    float3 p0 = mul(ObjectToWorld3x4(), float4(geometry.vertices[tri.x].position, 1.0));
    float3 p1 = mul(ObjectToWorld3x4(), float4(geometry.vertices[tri.y].position, 1.0));
    float3 p2 = mul(ObjectToWorld3x4(), float4(geometry.vertices[tri.z].position, 1.0));

    float3 normal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(normal, WorldRayDirection()) > 0.0)
        normal = -normal;

    float3 hitPoint = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 L = normalize(push.lightDir.xyz);
    float NdotL = saturate(dot(normal, L));

    switch (push.debugMode) {
    case kDebugNormals:
        payload.color = normal * 0.5 + 0.5;
        return;
    case kDebugHitDistance:
        payload.color = heatRamp(exp(-RayTCurrent() * 0.15));
        return;
    case kDebugPrimitiveId:
        payload.color = hashColor(InstanceIndex() * 7919u + PrimitiveIndex());
        return;
    case kDebugBarycentrics:
        payload.color = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
        return;
    default:
        break;
    }

    float visibility = 1.0;
    if (push.enableShadows != 0 && NdotL > 0.0)
        visibility = traceShadow(hitPoint + normal * 1e-3, L);

    if (push.debugMode == kDebugShadowRays) {
        // blue means no shadow ray was cast since the surface faces away from the light
        payload.color = NdotL <= 0.0 ? float3(0.1, 0.2, 0.8) : (visibility > 0.0 ? float3(1.0) : float3(0.8, 0.1, 0.1));
        return;
    }

    const float3 albedo = geometry.albedo.rgb;
    const float ambient = 0.15;
    payload.color = albedo * (ambient + (1.0 - ambient) * NdotL * visibility);
}
