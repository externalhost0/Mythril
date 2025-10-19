// BINDLESS SETS
// bindless aliasing does not work on moltenvk as of now (below)
// https://github.com/KhronosGroup/MoltenVK/issues/1975
// read about this problem (below)
// https://vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-roman-kuznetsov-meta.pdf


// sets 0-2
// TEXTURE DS
[[vk::binding(0, 0)]]
uniform Texture2D kTextures2D[];
[[vk::binding(0, 1)]]
uniform Texture3D kTextures3D[];
[[vk::binding(0, 2)]]
uniform TextureCube kTexturesCube[];
// SAMPLER DS
[[vk::binding(1, 0)]]
uniform SamplerState kSamplers[];
// STORAGE DS
[[vk::binding(2, 0)]]
uniform __DynamicResource kStorage[];


float4 textureBindless2D(uint textureid, uint samplerid, float2 uv) {
    return kTextures2D[NonUniformResourceIndex(textureid)].Sample(kSamplers[NonUniformResourceIndex(samplerid)], uv);
}
float4 textureBindless2DLod(uint textureid, uint samplerid, float2 uv, float lod) {
    return kTextures2D[NonUniformResourceIndex(textureid)].SampleLevel(kSamplers[NonUniformResourceIndex(samplerid)], uv, lod);
}
float4 textureBindlessCube(uint textureid, uint samplerid, float3 uvw) {
    return kTexturesCube[NonUniformResourceIndex(textureid)].Sample(kSamplers[NonUniformResourceIndex(samplerid)], uvw);
}
float4 textureBindlessCubeLod(uint textureid, uint samplerid, float3 uvw, float lod) {
    return kTexturesCube[NonUniformResourceIndex(textureid)].SampleLevel(kSamplers[NonUniformResourceIndex(samplerid)], uvw, lod);
}