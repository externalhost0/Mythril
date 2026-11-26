struct Inner {
    float3 v;
    float s;
};
struct Pointee {
    float4x4 m44;
    float3x4 m34;
    float4x3 m43;
    float3 v3;
    uint u;
    Inner inner;
};
struct RoundTripPush {
    Ptr<uint> out;
    Ptr<Pointee> pointee;
    float4x4 m44;
    float3x4 m34;
    float4 v4;
    float3 v3;
    int i;
    float2 v2;
    uint2 u2;
    Inner inner;
    float3 arr[2];
};

[vk::push_constant]
RoundTripPush push;

void put(inout uint n, uint value) {
    push.out[n] = value;
    n += 1;
}
void put(inout uint n, float value) { put(n, asuint(value)); }
void put(inout uint n, float2 v) { put(n, v.x); put(n, v.y); }
void put(inout uint n, float3 v) { put(n, v.x); put(n, v.y); put(n, v.z); }
void put(inout uint n, float4 v) { put(n, v.x); put(n, v.y); put(n, v.z); put(n, v.w); }
void put(inout uint n, Inner value) { put(n, value.v); put(n, value.s); }
void put(inout uint n, float4x4 m) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) put(n, m[r][c]); }
void put(inout uint n, float3x4 m) { for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) put(n, m[r][c]); }
void put(inout uint n, float4x3 m) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) put(n, m[r][c]); }

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main() {
    uint n = 0;
    put(n, push.m44);
    put(n, push.m34);
    put(n, push.v4);
    put(n, push.v3);
    put(n, asuint(push.i));
    put(n, push.v2);
    put(n, push.u2.x);
    put(n, push.u2.y);
    put(n, push.inner);
    put(n, push.arr[0]);
    put(n, push.arr[1]);
    Pointee p = push.pointee[0];
    put(n, p.m44);
    put(n, p.m34);
    put(n, p.m43);
    put(n, p.v3);
    put(n, p.u);
    put(n, p.inner);
}
