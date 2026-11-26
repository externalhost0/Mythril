struct Vertex {
    float3 position;
    float uv_x;
    float3 normal;
    float uv_y;
    float4 tangent;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float3 position;
};

struct ObjectData {
    float3 position;
    float3 color;
};
struct GlobalData {
    Ptr<ObjectData> objects;
    CameraData camera;
    float2 renderResolution;
    float time;
};
struct MaterialData {
    float3 tint;
    float warpIterations;
    float glowAmount;
    float distortAmount;
    uint unint1; // padding ig
};

struct GeometryPushConstant {
    float4x4 model;
    Ptr<Vertex> vertexBufferAddress;
    Ptr<GlobalData> perFrame;
    Ptr<MaterialData> perMaterial;
};

struct VSInput {
    uint VertexID : SV_VertexID;
    uint InstanceID : SV_InstanceID;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};
struct v2f {
    // system defined
    float4 ClipPos          : SV_Position;
    // user defined
    float3 Normal           : NORMAL;
    float2 UV               : TEXCOORD0;
    float3 WorldPosition    : POSITION;
};

[vk::push_constant]
GeometryPushConstant perObject;

[shader("vertex")]
v2f vs_main(VSInput input) {
    v2f output;

    Vertex v = perObject.vertexBufferAddress[input.VertexID];

    ObjectData obj = perObject.perFrame.objects[input.InstanceID];

    float3 worldpos = mul(perObject.model, float4(v.position, 1)).xyz + obj.position;
    output.ClipPos = mul(mul(perObject.perFrame.camera.proj, perObject.perFrame.camera.view), float4(worldpos, 1));

    output.Normal = mul(perObject.model, float4(v.normal, 0)).xyz;
    output.UV = float2(v.uv_x, v.uv_y);
    output.WorldPosition = worldpos;

    return output;
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;

    float2 resolution = perObject.perFrame.renderResolution;
    float time = perObject.perFrame.time;

    float2 fragCoord = input.UV * resolution;
    float2 uv = (2.0 * fragCoord - resolution) / min(resolution.x, resolution.y);
    
    float distort = perObject.perMaterial.distortAmount;
    for (float i = perObject.perMaterial.warpIterations; i < 10.0; i += 1.0) {
        uv.x += distort / i * cos(i * 2.5 * uv.y + time);
        uv.y += distort / i * cos(i * 1.5 * uv.x + time);
    }

    float glow = 0.1 / abs(sin(time - uv.y - uv.x));
    glow *= perObject.perMaterial.glowAmount;
    float3 color = glow.xxx * perObject.perMaterial.tint;
    output.FragColor = float4(color, 1.f);

    return output;
}