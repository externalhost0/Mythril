
struct HitPayload {
    float3 color = float3(0);
    float weight = 1;
    int depth = 0;
}

struct PushConstants {
    float3 cameraPosition;

}

[vk::push_constant]
PushConstants push;

[shader("raygeneration")]
void raygenMain()
{
    // Generate camera rays and trace them
    RayDesc ray;
    ray.Origin = push.cameraPosition;
    ray.Direction = normalize(rayDirection);
    TraceRay(topLevelAS, rayFlags, 0xff, 0, 0, 0, ray, payload);
}

[shader("closesthit")]
void hitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    // Basic material shading at intersection point
    payload.color = float3(1, 0, 0); // Simple red color
}

[shader("miss")]
void missMain(inout HitPayload payload)
{
    // Sky/background color for rays that miss geometry
    payload.color = float3(1, 1, 1); // White background
}