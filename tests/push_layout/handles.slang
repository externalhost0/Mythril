struct Holder { Texture2D.Handle h[2]; };
struct Push {
    SamplerState.Handle smp;
    Texture2D.Handle arr[2];
    Holder holder;
    Ptr<float> out;
};
[vk::push_constant] Push push;

[shader("compute")][numthreads(1,1,1)]
void cs_main() {
    push.out[0] = push.arr[1].SampleLevel(push.smp, float2(0), 0).x + push.holder.h[0].SampleLevel(push.smp, float2(0), 0).x;
}
