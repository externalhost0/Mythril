struct Pair<T> { T first; T second; };
struct Push { Pair<float> pf; Pair<uint2> pu; };
[vk::push_constant] Push push;
RWStructuredBuffer<float> o;
[shader("compute")][numthreads(1,1,1)]
void cs_main() { o[0] = push.pf.first + float(push.pu.first.x); }
