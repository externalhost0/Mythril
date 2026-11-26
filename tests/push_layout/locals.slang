struct Layer { Texture2D.Handle tex; uint2 scale; };
struct Push {
    Layer layers[2];
    Texture2D.Handle a;
    Texture2D.Handle b;
    SamplerState.Handle smp;
    Ptr<float> out;
};
[vk::push_constant] Push push;

[shader("compute")][numthreads(1,1,1)]
void cs_main(uint3 id: SV_DispatchThreadID) {
    Layer fromPush = push.layers[id.x & 1];
    Layer local[2] = { { push.a, uint2(1) }, { push.b, uint2(2) } };
    Layer fromLocal = local[id.y & 1];
    push.out[0] = fromPush.tex.SampleLevel(push.smp, float2(0), 0).x * float(fromPush.scale.x) + fromLocal.tex.SampleLevel(push.smp, float2(0), 0).x;
}
