// BINDLESS SETS
// bindless aliasing does not work on moltenvk as of now (below)
// https://github.com/KhronosGroup/MoltenVK/issues/1975
// read about this problem on slide 11 (below)
// https://vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-roman-kuznetsov-meta.pdf

// read about why we order the way we do (or why we dont)
//https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/03-convenience-features.html#:~:text=const%20T*.-,DescriptorHandle,-for%20Bindless%20Descriptor

// [[vk::binding(binding, set)]]
// we increment set # because of aliasing, once fixed remove increments

// sets 0, 1
// SAMPLER DS
// [[vk::binding(0, 0)]]
// uniform SamplerState kSamplers[];
//uniform StructuredBuffer<DescriptorHandle<SamplerState>> kSamplers;
// TEXTURE DS
// [[vk::binding(1, 0)]]
// uniform Texture2D kTextures2D[];
// //uniform StructuredBuffer<DescriptorHandle<Texture2D>> kTextures2D;
// [[vk::binding(1, 1)]]
// uniform Texture3D kTextures3D[];
// //uniform StructuredBuffer<DescriptorHandle<Texture3D>> kTextures3D;
// [[vk::binding(1, 2)]]
// uniform TextureCube kTexturesCube[];

// NEW WITH SLANG //

[vk::binding(0, 0)]
__DynamicResource<__DynamicResourceKind.Sampler> kSamplerHandles[];
[vk::binding(1, 0)]
__DynamicResource<__DynamicResourceKind.General> kTextureHandles[];
[vk::binding(2, 0)]
__DynamicResource<__DynamicResourceKind.General> kStorageHandles[];

float4 textureBindless2D(uint texId, uint samplerId, float2 uv) {
    return kTextureHandles[texId].asOpaqueDescriptor<Texture2D>().Sample(kSamplerHandles[samplerId].asOpaqueDescriptor<SamplerState>(), uv);
}
float4 textureBindless2DLOD(uint texId, uint samplerId, float2 uv, float lod) {
    return kTextureHandles[texId].asOpaqueDescriptor<Texture2D>().SampleLevel(kSamplerHandles[samplerId].asOpaqueDescriptor<SamplerState>(), uv, lod);
}
float4 textureBindlessCube(uint texId, uint samplerId, float3 uvw) {
    return kTextureHandles[texId].asOpaqueDescriptor<TextureCube>().Sample(kSamplerHandles[samplerId].asOpaqueDescriptor<SamplerState>(), uvw);
}
float4 textureBindlessCubeLOD(uint texId, uint samplerId, float3 uvw, float lod) {
    return kTextureHandles[texId].asOpaqueDescriptor<TextureCube>().SampleLevel(kSamplerHandles[samplerId].asOpaqueDescriptor<SamplerState>(), uvw, lod);
}
float4 rwtextureBindless2DLoad(uint texId, int2 location) {
    return kStorageHandles[texId].asOpaqueDescriptor<RWTexture2D>().Load(location);
}
void rwtextureBindless2DStore(uint texId, int2 location, float4 newValue) {
    kStorageHandles[texId].asOpaqueDescriptor<RWTexture2D>().Store(location, newValue);
}