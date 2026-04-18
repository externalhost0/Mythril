struct VSOutput {
    float4 pos : SV_Position;
};

struct FSOutput {
    float4 color : SV_Target0;
};

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID) {
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput output;
    output.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
FSOutput fs_main(VSOutput input) {
    FSOutput output;
    output.color = float4(1.0, 1.0, 1.0, 1.0);
    return output;
}
