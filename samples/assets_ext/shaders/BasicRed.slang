
import "Common.Math";

struct VSInput {
    uint VertexID : SV_VertexID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    // shader predefined
    float4 ClipPos : SV_Position;
};
struct Vertex {
    float3 position;
};


struct PushConstants {
    float4x4 mvp;
    Ptr<Vertex> vertexBufferAddress;
}

[[vk::push_constant]]
PushConstants perObject;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    output.ClipPos = float4(perObject.mvp * float4(v.position, 1));

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    output.FragColor = float4(1.0, 0.0, 0.0, 1.0);

    return output;
}