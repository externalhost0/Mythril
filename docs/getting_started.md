# Getting Started
`Mythril` makes it easy to quickly create and iterate over a new Vulkan application, only requiring you to give it a surface, for a specific chapter/feature check out one of the below sections.

- [Getting Started](#getting-started)
- [Background!](#background)
- [Creation](#creation)
- [Objects](#objects)
    - [Buffers](#buffers)
    - [Images](#images)
    - [Samplers](#samplers)
    - [Shaders](#shaders)
    - [Pipelines](#pipelines)
- [FrameGraph Usage](#framegraph)
- [CommandBuffer Usage](#commandbuffer)
- [Cleanup](#cleanup)


# Background!

`Mythril` is intended to be used as the primary way you are interacting with Vulkan and should not be mixed alongside other frameworks that control or have a heavy hand in rendering. Because it is fully bindless, you are also not given anyway to define descriptor sets and are expcted to use push constants frequently.

You are expected to manage the windowing system yourself, however `Mythril` is built to allow ANY windowing system as long as you can get an instance of VkSurfaceKHR from it, the examples use SDL3 and therefore show how you might retrieve it via other similar libraries.

The majority of operations you will be performing are the creation of our Vulkan objects and setting up your framegraph, and basically everything your application will do will take place inside the framegraph callbacks.

Throughout this document `Mythril::CTX` and `CTX` are synonymous, and lowercase "ctx" is just an instance of it.

# Creation

The `mythril::CTX` type is how you will primarily build out your app, it can only be instanced via the `mythril::CTXBuilder` as many important options need to be configured before we can build the Vulkan context. You should only ever create one `mythril::CTX`!

If you want a headless Vulkan context:
```cpp
auto ctx = mythril::CTXBuilder{}
.build();
```
However you likely want a Vulkan context that can present images to the screen, which means you **MUST** call `set_window_surface()`, it requires both the code necessary to create and destroy a `VkSurfaceKHR`, which mean

```cpp
auto ctx = mythril::CTXBuilder{}
.set_window_surface(
[sdlWindow](VkInstance instance) {
	VkSurfaceKHR surface;
	SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
	return surface;
},
[](VkInstance instance, VkSurfaceKHR surface_khr) {
	SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
})
.with_default_swapchain({
	.width = width,
	.height = height,
})
.build();
```
You may notice that we also call `with_default_swapchain` with whatever width and height our window is, it is not necessary to call this function from `mythril::CTXBuilder` however it does take care of more of the work. The above code will do the same thing if you defer swapchain creation as such:

```cpp
auto ctx = mythril::CTXBuilder{}
.set_window_surface(
[sdlWindow](VkInstance instance) {
	VkSurfaceKHR surface;
	SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
	return surface;
},
[](VkInstance instance, VkSurfaceKHR surface_khr) {
	SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
})
.build();
ctx->createSwapchain({
	.width = width,
	.height = height
});
```

Once you have your `mythril::CTX` instance you can do anything!

# Objects

All objects are created through `mythril::CTX` and returned as RAII wrapper types. 
Resources are automatically destroyed when their wrapper goes out of scope. 
If you need early explicit cleanup, every wrapper exposes a `CTX::destroy(handle)` path but is only necessary in some edge cases and is generally reccomended to avoid using.

## Buffers

Buffers store arbitrary GPU data: vertex data, storage buffers, indirect draw arguments, etc. Create one with `CTX::createBuffer()`.

```cpp
// Device-local buffer with initial data
mythril::Buffer vertexBuffer = ctx->createBuffer({
    .size      = sizeof(Vertex) * vertices.size(),
    .usage     = mythril::BufferUsageBits_Storage,
    .storage   = mythril::StorageType::Device,
    .initialData = vertices.data(),
    .debugName = "Vertex Buffer",
});

// Device-local buffer for per-frame data accessible by shaders
mythril::Buffer uniformBuffer = ctx->createBuffer({
    .size    = sizeof(FrameData),
    .usage   = mythril::BufferUsageBits_Storage,
    .storage = mythril::StorageType::Device,
    .debugName = "Frame Data",
});
```

**`usage` flags** (combine with `|`):

| Flag | Use |
|---|---|
| `BufferUsageBits_Index` | Index buffer |
| `BufferUsageBits_Storage` | Shader read/write storage buffer |
| `BufferUsageBits_Indirect` | Indirect draw/dispatch arguments |

**`storage` types**:

| Type | Description |
|---|---|
| `StorageType::Device` | GPU-only. Fastest for GPU access. Cannot be CPU-mapped. |
| `StorageType::HostVisible` (DEFAULT) | CPU-accessible. Use for staging or frequently-updated data. |
| `StorageType::Memoryless` | Transient on-chip memory. |

**Good to Knows**:
- `initialData` performs a one-time upload at creation. 
- You can update buffers via both `CTX::upload()` which has no size limit and `CommandBuffer::cmdUpdateBuffer()` which has a max of 64kb.
- `Buffer::gpuAddress()` is only valid on storage buffers with `StorageType::Device`.
- To read data back to CPU use `CTX::download()`.

---

## Images

Images (textures) represent 2D, 3D, or cubemap GPU images. Create one with `CTX::createTexture()`.

```cpp
// Color attachment for off-screen rendering
mythril::Texture colorTarget = ctx->createTexture({
    .dimension = {1280, 720, 1},
    .format    = VK_FORMAT_R8G8B8A8_UNORM,
    .usage     = mythril::TextureUsageBits_Attachment,
    .debugName = "Color Target",
});

// Sampled texture loaded from data with auto-generated mipmaps
mythril::Texture albedo = ctx->createTexture({
    .dimension       = {512, 512, 1},
    .format          = VK_FORMAT_R8G8B8A8_SRGB,
    .usage           = mythril::TextureUsageBits_Sampled,
    .numMipLevels    = 10,
    .initialData     = pixelData,
    .generateMipmaps = true,
    .debugName       = "Albedo",
});

// Depth attachment
mythril::Texture depthTarget = ctx->createTexture({
    .dimension = {1280, 720, 1},
    .format    = VK_FORMAT_D32_SFLOAT,
    .usage     = mythril::TextureUsageBits_Attachment,
    .debugName = "Depth Target",
});
```

**`usage` flags** (combine with `|`), you must have at least one flag:

| Flag | Use |
|---|---|
| `TextureUsageBits_Sampled` | Sample in shaders via a sampler |
| `TextureUsageBits_Storage` | Read/write as storage image in shaders |
| `TextureUsageBits_Attachment` | Color or depth/stencil render target |

**`type` values**:

| Type | Notes                                              |
|---|----------------------------------------------------|
| `TextureType::Type_2D` (DEFAULT) | Standard 2D texture or array when `numLayers > 1`. |
| `TextureType::Type_3D` | Volumetric. Set `dimension.depth > 1`.             |
| `TextureType::Type_Cube` | Cubemap. Requires `numLayers = 6`.                 |

**Texture views** access a subresource (specific mip or layer) without creating a new texture:

```cpp
auto viewKey = texture.createView({
    .mipLevel    = 2,
    .numMipLevels = 1,
    .debugName   = "Mip 2 View",
});
TextureHandle viewHandle = texture.handle(viewKey);
```

**Good to Knows**:
- `generateMipmaps = true` requires `initialData != nullptr`. If no initial data is provided, mipmaps will not be generated.
- Swapchain images are not user-owned, never call `CTX::destroy()` on a handle retrieved from the swapchain.
- Cubemaps must set `type = TextureType::Type_Cube` **and** `numLayers = 6`. Missing either will produce incorrect results.
- MSAA textures (`samples > X1`) used as attachments typically need a resolve step before sampling.
- Utilize `StorageType::Memoryless` for transient attachments, usually for MSAA targets.
---

## Samplers

Samplers describe how a shader reads from a texture; filtering, wrapping, and optional depth comparison. Create one with `CTX::createSampler()`.

```cpp
// Standard trilinear sampler
mythril::Sampler trilinear = ctx->createSampler({
    .magFilter = mythril::SamplerFilter::Linear,
    .minFilter = mythril::SamplerFilter::Linear,
    .mipMap    = mythril::SamplerMipMap::Linear,
    .debugName = "Trilinear Sampler",
});

// Shadow map / PCF sampler
mythril::Sampler shadowSampler = ctx->createSampler({
    .depthCompareEnabled = true,
    .depthCompareOp      = mythril::CompareOp::LessEqual,
    .debugName           = "Shadow Sampler",
});

// Anisotropic sampler for surfaces at oblique angles
mythril::Sampler aniso = ctx->createSampler({
    .mipMap        = mythril::SamplerMipMap::Linear,
    .anistrophic   = true,
    .maxAnisotropic = 8,
    .debugName      = "Anisotropic Sampler",
});
```

All fields are optional.

**Filter / mipmap**:

| Option | Vulkan Definition              |
|---|--------------------------------|
| `SamplerFilter::Nearest` | VK_FILTER_NEAREST              |
| `SamplerFilter::Linear` (DEFAULT) | VK_FILTER_LINEAR               |
| `SamplerMipMap::Disabled` (DEFAULT) | No Mipmapping!                 |
| `SamplerMipMap::Nearest` | VK_SAMPLER_MIPMAP_MODE_NEAREST |
| `SamplerMipMap::Linear` | VK_SAMPLER_MIPMAP_MODE_LINEAR  |

**Wrap modes** (configurable per U/V/W axis):

| Mode | Vulkan Definition |
|---|--|
| `SamplerWrap::Repeat` (DEFAULT) | VK_SAMPLER_ADDRESS_MODE_REPEAT |
| `SamplerWrap::MirrorRepeat` | VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT |
| `SamplerWrap::ClampEdge` | VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE |
| `SamplerWrap::ClampBorder` | VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER |
| `SamplerWrap::MirrorClampEdge` | VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE |

---

## Shaders

Shaders are written in [Slang](https://shader-slang.com/) and compiled to SPIR-V at load time. Create one with `CTX::createShader()`.

```cpp
mythril::Shader defaultShader = ctx->createShader({
    .filePath  = kShaderDir / "PBR.slang",
    .debugName = "PBR Shader",
});
```

A single `.slang` file can contain both vertex and fragment entry points, there is no requirement to split them into separate files or separate `Shader` objects.

**Good to Knows**:
- Keep the `Shader` alive for as long as any pipeline that uses it exists. Destroying the shader while a pipeline still references it is undefined behavior.
- Slang include search paths and compiler options are configured via `CTXBuilder::set_slang_cfg()` before calling `CTXBuilder::build()`, use these if your shader or its imports can't be found.

---

## Pipelines

Pipelines bind shaders and fixed-function state. Mythril supports graphics and compute pipelines as of now.

### Graphics Pipeline

```cpp
mythril::GraphicsPipeline pipeline = ctx->createGraphicsPipeline({
    .vertexShader   = {defaultShader},
    .fragmentShader = {defaultShader},
    .topology       = mythril::TopologyMode::TRIANGLE,
    .polygon        = mythril::PolygonMode::FILL,
    .blend          = mythril::BlendingMode::ALPHA_BLEND,
    .cull           = mythril::CullMode::BACK,
    .multisample    = mythril::SampleCount::X1,
    .debugName      = "Mesh Pipeline",
});
```

`ShaderStage` accepts a `Shader` object directly (as above) or a `ShaderHandle`. To target a specific entry point:

```cpp
.vertexShader = {defaultShader.handle(), "vertMain"},
```

**Topology**:

| Mode | Vulkan Definition                   |
|---|-------------------------------------|
| `TopologyMode::TRIANGLE` (DEFAULT) | VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST |
| `TopologyMode::LIST` | VK_PRIMITIVE_TOPOLOGY_LINE_LIST     |
| `TopologyMode::STRIP` | VK_PRIMITIVE_TOPOLOGY_LINE_STRIP    |

**Blending**:

| Mode | Use |
|---|---|
| `BlendingMode::OFF` (DEFAULT) | No blending |
| `BlendingMode::ALPHA_BLEND` | Standard transparency |
| `BlendingMode::ADDITIVE` | Glow / particles |
| `BlendingMode::MULTIPLY` | Darkening |
| `BlendingMode::MASK` | Stencil mask |

**Cull**:

| Mode | Vulkan Definition      |
|---|------------------------|
| `CullMode::OFF` (DEFAULT) | VK_CULL_MODE_NONE      |
| `CullMode::BACK` | VK_CULL_MODE_BACK_BIT  |
| `CullMode::FRONT` | VK_CULL_MODE_FRONT_BIT |

**Polygon**:

| Mode | Vulkan Definition    |
|---|----------------------|
| `PolygonMode::FILL` (DEFAULT) | VK_POLYGON_MODE_FILL |
| `PolygonMode::LINE` | VK_POLYGON_MODE_LINE |

### Compute Pipeline

```cpp
mythril::ComputePipeline compute = ctx->createComputePipeline({
    .shader    = computeShader.handle(),
    .debugName = "Particle Update",
});
```

### Specialization Constants

Both pipeline types support up to 16 specialization constants, which let you bake values into the SPIR-V at pipeline creation time without recompiling the shader:

```cpp
uint32_t maxLights = 8;
mythril::GraphicsPipeline pipeline = ctx->createGraphicsPipeline({
    .vertexShader   = {shader},
    .fragmentShader = {shader},
    .specConstants  = {
        mythril::SpecializationConstantEntry{&maxLights, sizeof(maxLights), "MAX_LIGHTS"},
    },
    .debugName = "PBR Pipeline",
});
```

**Good to Knows**:
- Multiple pipelines can share a single `Shader`, as there is no need to duplicate shader objects per pipeline.

# Cleanup

There is no manual cleanup necessary, as `Mythril`'s Vulkan objects will call the necessary destruction logic when their destructor is called. The same goes for `mythril::CTX` which will perform cleanup for all previously created objects if their destructors have not already been called.

What you do have to do is keep the windowing system/context alive until AFTER `mythril::CTX` is destroyed, as destroying your window, i.e. `SDL_WINDOW*` too early will result in validation errors. This is why all the samples scope `mythril::CTX` but not the created window.