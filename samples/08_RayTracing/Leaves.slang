import Scene;

struct LeavesPushConstant {
    Ptr<Instance> leaves;
    uint64_t leafBlas;
    float3 crown0;
    float crownRadius;
    float3 crown1;
    float floorHeight;
    float2 windDirection;
    float time;
    uint leafCount;
}

[[vk::push_constant]]
LeavesPushConstant push;

float hash(uint n) {
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffff);
}

float3x3 axisAngle(float3 axis, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    float t = 1.0 - c;
    return float3x3(
        t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z, t * axis.x * axis.z + s * axis.y,
        t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,          t * axis.y * axis.z - s * axis.x,
        t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x, t * axis.z * axis.z + c);
}

static const float kPi = 3.14159265;
static const float kAirborneLifeFraction = 0.75;

[shader("compute")]
[numthreads(32, 1, 1)]
void cs_main(uint3 id: SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= push.leafCount)
        return;
    float life = 5.0 + 4.0 * hash(i * 8u + 0u);
    float u = frac((push.time + life * hash(i * 8u + 1u)) / life);
    float3 crown = (i & 1u) == 0u ? push.crown0 : push.crown1;
    float3 spawn = crown + (float3(hash(i * 8u + 2u), hash(i * 8u + 3u), hash(i * 8u + 4u)) * 2.0 - 1.0) * push.crownRadius * float3(1.0, 0.5, 1.0);

    float3 wind = float3(push.windDirection.x, 0.0, push.windDirection.y);
    float3 across = float3(-wind.z, 0.0, wind.x);
    float s = saturate(u / kAirborneLifeFraction);
    float distance = s * (6.0 + 4.0 * hash(i * 8u + 5u));
    float3 position = spawn + wind * distance + across * sin(s * 7.0 + float(i)) * 0.7;
    position.y = lerp(spawn.y, push.floorHeight + 0.02, s * s) + sin(s * kPi) * (0.4 + 0.8 * hash(i * 8u + 6u));

    float3 axis = normalize(float3(hash(i + 101u), hash(i + 211u), hash(i + 307u)) * 2.0 - 1.0 + float3(0, 0.01, 0));
    float tumble = push.time * (3.0 + 4.0 * hash(i + 409u));
    float yaw = 2.0 * kPi * hash(i + 503u);
    float3x3 rotation = u < kAirborneLifeFraction ? axisAngle(axis, tumble) : axisAngle(float3(0, 1, 0), yaw);
    float scale = 0.16 * saturate(u / 0.04) * saturate((1.0 - u) / 0.06) + 1e-4;
    float3x3 m = rotation * scale;

    Instance leaf;
    leaf.row0 = float4(m[0], position.x);
    leaf.row1 = float4(m[1], position.y);
    leaf.row2 = float4(m[2], position.z);
    leaf.customIndexAndMask = kGeometryLeaf | (0xFFu << 24);
    leaf.sbtOffsetAndFlags = 0u;
    leaf.blas = push.leafBlas;
    push.leaves[i] = leaf;
}
