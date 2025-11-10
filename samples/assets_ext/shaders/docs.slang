struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV      : TEXCOORD0;
};
struct SomeData {
    float time;
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> sampler;
}
ParameterBlock<SomeData> param;

[shader("vertex")]
v2f vs_main(uint VertexID : SV_VertexID) {
    v2f output;
    output.UV = float2((VertexID << 1) & 2, VertexID & 2 );
    output.ClipPos = float4(output.UV * 2.0 + -1.0, 0.0, 1.0);
    output.UV.y = 1.0 - output.UV.y;
    return output;
}
[shader("pixel")]
float4 fs_main(v2f input) {

    float4 col = param.texture.Sample(param.sampler, input.UV);
    col *= sin(param.time);
    
    return col;
}