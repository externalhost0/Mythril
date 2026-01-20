import GPUStructs;

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
    float4 FragNormals : SV_Target1;
}

struct PushConstant {
    DescriptorHandle<Texture2D> depthTexture;
    DescriptorHandle<SamplerState> sampler;
}

[[vk::push_constant]]
PushConstant push;

ParameterBlock<FrameData> frame;

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

static float LinearizeDepth(float depth, float zNear, float zFar) {
    return zNear * zFar / (zFar + depth * (zNear - zFar));
}
static float LinearizeDepthReverseZ(float depth, float zNear, float zFar) {
    // return zNear * zFar / (depth * (zNear - zFar) + zFar);
    return zNear * zFar / (zFar - depth * (zFar - zNear));
}


float3 GetViewPosition(float2 uv, float depth) {
    // CONCEPT: Converting screen space → view space
    // 1. UV (0→1) to NDC (-1→1)
    // 2. Apply inverse projection matrix
    // 3. Perspective divide (w component)

    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = mul(frame.camera.invProj, clipPos);
    return viewPos.xyz / viewPos.w;
}
float3 ReconstructNormalFromDepth_Samples(
    float2 uv,
    float2 texelSize
) {
    // Sample depth in a cross pattern
    //     N
    //   W C E
    //     S

    float depthC = push.depthTexture.Sample(push.sampler, uv).r;
    float depthN = push.depthTexture.Sample(push.sampler, uv + float2(0, texelSize.y)).r;
    float depthS = push.depthTexture.Sample(push.sampler, uv - float2(0, texelSize.y)).r;
    float depthE = push.depthTexture.Sample(push.sampler, uv + float2(texelSize.x, 0)).r;
    float depthW = push.depthTexture.Sample(push.sampler, uv - float2(texelSize.x, 0)).r;

    float3 posC = GetViewPosition(uv, depthC);
    float3 posN = GetViewPosition(uv + float2(0, texelSize.y), depthN);
    float3 posS = GetViewPosition(uv - float2(0, texelSize.y), depthS);
    float3 posE = GetViewPosition(uv + float2(texelSize.x, 0), depthE);
    float3 posW = GetViewPosition(uv - float2(texelSize.x, 0), depthW);

    // Calculate horizontal and vertical differences
    float3 dx = posE - posW;
    float3 dy = posN - posS;

    // Cross product for normal
    float3 normal = normalize(cross(dy, dx));
    return normal;
}
float3 GetViewNormal(float2 uv) {
    uint2 dims;
    push.depthTexture.GetDimensions(dims.x, dims.y);
    return ReconstructNormalFromDepth_Samples(uv, 1.f / float2(dims));
}

float3 ReconstructNormalFromDepth_Derivatives(float2 uv, float centerDepth, float4x4 invProj) {
    // Reconstruct view-space position for center pixel
    float4 clipPos = float4(uv * 2.0 - 1.0, centerDepth, 1.0);
    float4 viewPos = mul(invProj, clipPos);
    viewPos.xyz /= viewPos.w;

    // Use screen-space derivatives to get tangent vectors
    float3 viewPosDX = ddx(viewPos.xyz);
    float3 viewPosDY = ddy(viewPos.xyz);

    // Cross product gives the normal
    float3 normal = normalize(cross(viewPosDY, viewPosDX));
    return normal;
}


float3 UVToView(float2 UV, float viewDepth) {
    UV = frame.camera.g_f2UVToViewA * UV + frame.camera.g_f2UVToViewB;
    return float3(UV * viewDepth, viewDepth);
}
float3 FetchFullResViewPos(float2 uv) {
    float d = push.depthTexture.SampleLevel(push.sampler, uv, 0).r;
    float viewDepth = LinearizeDepth(d, frame.camera.near, frame.camera.far);
    return UVToView(uv, viewDepth);
}
float3 MinDiff(float3 P, float3 Pr, float3 Pl) {
    float3 V1 = Pr - P;
    float3 V2 = P - Pl;
    return (dot(V1, V1) < dot(V2, V2)) ? V1 : V2;
}
float3 ReconstructNormal(float2 uv, float3 pos) {
    uint2 fullRes;
    push.depthTexture.GetDimensions(fullRes.x, fullRes.y);
    float2 g_f2InvFullResolution = float2(1 / fullRes.x, 1 / fullRes.y);

    float3 Pr = FetchFullResViewPos(uv + float2(g_f2InvFullResolution.x, 0));
    float3 Pl = FetchFullResViewPos(uv + float2(-g_f2InvFullResolution.x, 0));
    float3 Pt = FetchFullResViewPos(uv + float2(0, g_f2InvFullResolution.y));
    float3 Pb = FetchFullResViewPos(uv + float2(0, -g_f2InvFullResolution.y));
    if (abs(Pr.z - pos.z) > 0.1) Pr = pos;
    if (abs(Pl.z - pos.z) > 0.1) Pl = pos;
    if (abs(Pt.z - pos.z) > 0.1) Pt = pos;
    if (abs(Pb.z - pos.z) > 0.1) Pb = pos;
    float3 dx = MinDiff(pos, Pr, Pl);
    float3 dy = MinDiff(pos, Pt, Pb);
    return normalize(cross(dy, dx));
}
float4 ReconstructNormal_PS(float2 uv) {
    float3 ViewPosition = FetchFullResViewPos(uv);
    float3 ViewNormal = ReconstructNormal(uv, ViewPosition);
    return float4(ViewNormal * 0.5 + 0.5, 0);
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float depth = push.depthTexture.Sample(push.sampler, input.UV).r;
    float linearDepth = LinearizeDepthReverseZ(depth, frame.camera.near, frame.camera.far);
    output.FragColor = float4(float3(linearDepth), 1);
    output.FragNormals = float4(ReconstructNormalFromDepth_Derivatives(input.UV, depth, frame.camera.invProj), 1);
    return output;
}