struct Push {
    Texture2D.Handle tex;
    RWTexture2D<float4>.Handle img;
    SamplerState.Handle smp;
    Ptr<float> out;
};
[vk::push_constant] Push push;

[noinline] float4 sampleIt(Texture2D.Handle t, SamplerState.Handle s) { return t.SampleLevel(s, float2(0), 0); }
[noinline] RWTexture2D<float4>.Handle pick(RWTexture2D<float4>.Handle h) { return h; }
[noinline] void writeOut(Ptr<float> p, float v) { p[0] = v; }

[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    float4 c = sampleIt(push.tex, push.smp);
    pick(push.img)[uint2(0)] = c;
    writeOut(push.out, c.x);
}
