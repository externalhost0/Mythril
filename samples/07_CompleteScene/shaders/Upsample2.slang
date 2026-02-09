import GPUStructs;

struct PushConstants {
    DescriptorHandle<Texture2D> inTexture;
    DescriptorHandle<RWTexture2D> outTexture;
    DescriptorHandle<SamplerState> sampler;
    int2 outSize;
    float filterRadius;
    float blend;
};

[vk::push_constant]
PushConstants push;

groupshared float3 sTile[10][10]; // 8x8 group + 1px halo on all sides

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(
    uint3 groupId       : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 dispatchId    : SV_DispatchThreadID)
{
    uint2 outputCoord = dispatchId.xy;

    if (outputCoord.x >= uint(push.outSize.x) ||
        outputCoord.y >= uint(push.outSize.y)) {
        return;
    }

    float2 invOutSize = 1.0 / float2(push.outSize);
    float2 uv = (float2(outputCoord) + 0.5) * invOutSize;

    float2 dx = float2(push.filterRadius, 0) * invOutSize;
    float2 dy = float2(0, push.filterRadius) * invOutSize;

    uint lx = groupThreadId.x + 1;
    uint ly = groupThreadId.y + 1;

    // Center
    sTile[lx][ly] = push.inTexture.SampleLevel(push.sampler, uv, 0).rgb;

    // Halo loads
    if (groupThreadId.x == 0)
        sTile[0][ly] = push.inTexture.SampleLevel(push.sampler, uv - dx, 0).rgb;

    if (groupThreadId.x == 7)
        sTile[9][ly] = push.inTexture.SampleLevel(push.sampler, uv + dx, 0).rgb;

    if (groupThreadId.y == 0)
        sTile[lx][0] = push.inTexture.SampleLevel(push.sampler, uv - dy, 0).rgb;

    if (groupThreadId.y == 7)
        sTile[lx][9] = push.inTexture.SampleLevel(push.sampler, uv + dy, 0).rgb;

    // Corners
    if (groupThreadId.x == 0 && groupThreadId.y == 0)
        sTile[0][0] = push.inTexture.SampleLevel(push.sampler, uv - dx - dy, 0).rgb;

    if (groupThreadId.x == 7 && groupThreadId.y == 0)
        sTile[9][0] = push.inTexture.SampleLevel(push.sampler, uv + dx - dy, 0).rgb;

    if (groupThreadId.x == 0 && groupThreadId.y == 7)
        sTile[0][9] = push.inTexture.SampleLevel(push.sampler, uv - dx + dy, 0).rgb;

    if (groupThreadId.x == 7 && groupThreadId.y == 7)
        sTile[9][9] = push.inTexture.SampleLevel(push.sampler, uv + dx + dy, 0).rgb;

    GroupMemoryBarrierWithGroupSync();

    float3 a = sTile[lx - 1][ly + 1];
    float3 b = sTile[lx    ][ly + 1];
    float3 c = sTile[lx + 1][ly + 1];

    float3 d = sTile[lx - 1][ly];
    float3 e = sTile[lx    ][ly];
    float3 f = sTile[lx + 1][ly];

    float3 g = sTile[lx - 1][ly - 1];
    float3 h = sTile[lx    ][ly - 1];
    float3 i = sTile[lx + 1][ly - 1];

    float3 result = e * 4.0;
    result += (b + d + f + h) * 2.0;
    result += (a + c + g + i);
    result *= 1.0 / 16.0;

    float3 prev = push.outTexture[outputCoord].rgb;
    push.outTexture[outputCoord] = float4(prev + result, 1.0);
}
