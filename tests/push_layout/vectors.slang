struct Push {
    float4 f;
    half4 h;
    int16_t4 s;
    double2 d;
    half2x2 m;
    uint4 u;
    Ptr<float> o;
};
[vk::push_constant] Push push;

[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    push.o[0] = push.f.x + float(push.h.y) + float(push.s.z) + float(push.d.x) + float(push.m[1][0]) + float(push.u.w);
}
