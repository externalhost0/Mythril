struct AccelPush {
    RaytracingAccelerationStructure.Handle tlas;
    uint mask;
    float tMax;
    Ptr<float> out;
};
[vk::push_constant] AccelPush push;

[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    RayDesc ray = { float3(0), 0, float3(0, 0, 1), push.tMax };
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(RaytracingAccelerationStructure(push.tlas), RAY_FLAG_NONE, push.mask, ray);
    query.Proceed();
    push.out[0] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1.0 : 0.0;
}
