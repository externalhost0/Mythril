struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
}

struct FullscreenPushConstant {
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> sampler;
}

[[vk::push_constant]]
FullscreenPushConstant push;


[shader("vertex")]
v2f vs_main(uint VertexID : SV_VertexID) {
    v2f output;
    // generates the fullscreen triangle without any cpu interaction
    // https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html

    output.UV = float2((VertexID << 1) & 2, VertexID & 2 );
    output.ClipPos = float4(output.UV * 2.0 + -1.0, 0.0, 1.0);
    // flip the y axis
    output.UV.y = 1.0 - output.UV.y;
    return output;
}

// for debugging with perspective camera
float linearizeDepth(float vertexZ, float far, float near) {
    float ndc = vertexZ; // already in [-1,1]
    float linearDepth = (2.0 * near * far) / (far + near - ndc * (far - near));
    return linearDepth / far;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    output.FragColor = float4(push.texture.Sample(push.sampler, input.UV).r, 0, 0, 1);
    return output;
}