[vk::constant_id(3)] const int kCount = 4;
[vk::constant_id(7)] const float kScale = 1.0;
[SpecializationConstant] const uint kAuto = 2;
[vk::constant_id(9)] const bool kFlag = false;
struct SpecPush { Ptr<float> o; }; [vk::push_constant] SpecPush push;
[shader("vertex")] float4 vs_main(uint id : SV_VertexID) : SV_Position { return float4(kCount * kScale + kAuto + (kFlag ? 1 : 0), 0, 0, 1); }
[shader("fragment")] float4 fs_main() : SV_Target { return float4(kScale, 0, 0, 1); }
[shader("compute")][numthreads(1,1,1)] void cs_main() { push.o[0] = kScale; }
