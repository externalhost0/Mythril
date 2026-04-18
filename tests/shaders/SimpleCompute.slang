#include "TestGPUStructs.h"

[[vk::push_constant]]
ComputePush push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < push.count) {
        push.outputBuffer[tid.x] = tid.x * 2;
    }
}
