#include "GPUStructs.h"

[[vk::push_constant]]
FullscreenPushConstant push;

struct v2f {
    float4 ClipPos : SV_Position;
};

[shader("vertex")]
v2f vs_main(uint vertexID : SV_VertexID) {
    v2f output;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.ClipPos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("pixel")]
float4 fs_main(v2f input) : SV_Target0 {
    // pixel-exact load, so the y flip on the fullscreen triangle doesn't matter
    return push.rayTracedImage.Load(int3(int2(input.ClipPos.xy), 0));
}
