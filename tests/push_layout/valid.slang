struct Node { Ptr<Node> next; float value; uint pad0; };
struct Inner { float3 v; float s; };
struct Target {
    double d;
    half h;
    int16_t i16;
    uint flags;
    float3 arr3[3];
    Inner inners[2];
    float3x4 m34;
    float4x3 m43;
    float3x4 rm34;
    float3x3 m33;
    uint64_t big;
};
struct Push {
    Ptr<Target> target;
    Ptr<Node> list;
    Ptr<float4x4> matrices;
    float3x4 pm34;
    Inner inner;
    Texture2D.Handle tex;
    RWTexture2D<float4>.Handle img;
    SamplerState.Handle smp;
    Texture2D.Handle texArr[3];
    uint mode;
    float last;
};
[vk::push_constant] Push push;
[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    Target t = push.target[0];
    t.d += 1; t.h += half(1); t.i16 += 1; t.flags += push.mode; t.big += 1;
    float4x4 m = push.matrices[1];
    float x = push.list.next.value + push.pm34[1][2] + push.inner.s + push.last + m[0][0]
        + push.tex.Sample(push.smp, float2(0)).x + push.texArr[2].Sample(push.smp, float2(0)).x;
    t.arr3[2] += x + t.inners[1].v + t.m34[2].xyz + t.m43[3] + t.rm34[1].xyz + t.m33[2];
    push.img[uint2(0)] = float4(x);
    push.target[0] = t;
}
