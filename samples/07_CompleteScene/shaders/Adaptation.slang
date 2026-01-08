import GPUStructs;

struct PushConstants {
    DescriptorHandle<RWTexture2D> currentSceneLumTex;
    DescriptorHandle<RWTexture2D> prevAdaptedLumTex;
    DescriptorHandle<RWTexture2D> adaptedOutTex;
    float adaptionSpeed;
    float pad;
}

[vk::push_constant]
PushConstants push;

[shader("compute")]
[numthreads(1, 1)]
void cs_main(uint3 threadId: SV_DispatchThreadID) {
    float lum_current = push.currentSceneLumTex.Load(int2(0, 0)).x;
    float lum_previous = push.prevAdaptedLumTex.Load(int2(0, 0)).x;
    float factor = 1.0 - exp(-push.adaptionSpeed);
    float new_adaptation = lum_previous + (lum_current - lum_previous) * factor;
    push.adaptedOutTex.Store(int2(0, 0), float4(new_adaptation));
}