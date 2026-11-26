struct SquarePush {
    float4x4 m;
    Ptr<float4> out;
};
[vk::push_constant] SquarePush push;

[shader("compute")][numthreads(1,1,1)]
void cs_main() { *push.out = mul(push.m, float4(1, 2, 3, 4)); }
