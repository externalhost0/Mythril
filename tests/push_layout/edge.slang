enum class Mode : uint { A, B };
struct Inner { float3 v; float s; };
struct Target {
    float a;
    float3 b;
    double d;
    half h;
    int16_t i16;
    bool flag;
    Mode mode;
    float3 arr3[3];
    Inner inners[2];
    float3x4 m34;
    float4x3 m43;
    column_major float3x4 rm34;
    float3x3 m33;
    uint64_t big;
    float tail;
};
struct Push {
    float x;
    Ptr<Target> target;
    float3x4 pm34;
    bool pflag;
    Inner inner;
    uint64_t u64;
    float last;
};
[vk::push_constant] Push push;

[shader("compute")][numthreads(1,1,1)]
void cs_main(uniform float4 epUniform) {
    Target t = push.target[0];
    t.a = push.x + epUniform.x + push.pm34[0][1] + (push.pflag ? 1 : 0) + push.inner.s + float(push.u64) + push.last;
    t.b += t.arr3[2] + t.inners[1].v + t.m34[2].xyz + t.m43[3] + t.rm34[1].xyz + t.m33[2];
    t.d += 1; t.h += half(1); t.i16 += 1; t.flag = !t.flag; t.mode = Mode.B; t.big += 1; t.tail += 1;
    push.target[0] = t;
}
