namespace a { struct Light { float3 pos; }; }
namespace b { struct Light { float4 color; }; }
struct Pair<T> { T first; T second; };
struct Push {
    Ptr<a::Light> lightsA;
    Ptr<b::Light> lightsB;
    Pair<float> pf;
    Pair<uint2> pu;
};
[vk::push_constant] Push push;
[shader("compute")][numthreads(1,1,1)]
void cs_main() { push.lightsA[0].pos = push.lightsB[0].color.xyz + push.pf.first + float(push.pu.first.x); }
