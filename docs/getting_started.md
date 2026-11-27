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
- [RenderGraph Usage](#rendergraph)
- [Cleanup](#cleanup)


# Background!

`Mythril` is intended to be used as the primary way you are interacting with Vulkan and should not be mixed alongside other frameworks that control or have a heavy hand in rendering. Because it is fully bindless, you are also not given any way to define descriptor sets and are expcted to use push constants frequently.

You are expected to manage the windowing system yourself, however `Mythril` is built to allow ANY windowing system as long as you can get an instance of VkSurfaceKHR from it, the examples use SDL3 and therefore show how you might retrieve it via other similar libraries.

The majority of operations you will be performing are the creation of our Vulkan objects and setting up your rendergraph, and basically everything your application submits to the GPU will take place inside the rendergraph's setup/execute lambdas, [read more](#rendergraph).

Throughout this document `mythril::CTX` and `CTX` are synonymous, and lowercase "ctx" is just an instance of it.

# Creation

The `mythril::CTX` type is how you will primarily build out your app, it can only be instanced via the `mythril::CTXBuilder` as many important options need to be configured before we can build the Vulkan context. You should only ever create one `mythril::CTX`!

If you want a headless Vulkan context:
```cpp
auto ctx = mythril::CTXBuilder{}
.build();
```
However you likely want a Vulkan context that can present images to the screen, which means you **MUST** call `set_window_surface()`, it requires both the code necessary to create and destroy a `VkSurfaceKHR`, which looks like:

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
If you need early explicit cleanup, every wrapper exposes a `CTX::destroy(handle)` path but is only necessary in some edge cases and is generally recommended to avoid using.
If you want to manually manage your objects you can also call `.release()` on any object, this will return the actual handle of the object and allow you to move it around without it being destroyed after leaving scope. 

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

**Good to Knows**:
- `initialData` performs a one-time upload at creation. 
- Though `StorageType::HostVisible` is the default storage setting (for working out of the box reasons), it is reccomend to use `StorageType::Device` since you likely don't need CPU accessible data.
- `Buffer::gpuAddress()` is only valid on storage buffers with `StorageType::Device`.
- To read data back to CPU use `CTX::download()`.
- You can update buffers via `CTX::upload()` which has no size limit *(reccomended for one-time uploads)*. For per-frame updates use `TransferBuilder::update()` or `TransferBuilder::upload()` instead, [read more](#add-transfer-pass). `ComputeCmd::cmdUpdateBufferEXT()` also exists inside compute passes, but only takes a single trivially copyable struct of at most 64kb.
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
- Sampled-only textures are already shared across every queue family in use. Attachments and storage images stay exclusive so they keep compression, and the graph transfers their ownership between queues for you. `concurrent = true` makes them shared too, with no ownership transfer. (this can cost compression on AMD hardware)
- Graph textures infer `StorageType::Memoryless` for attachments that never leave one rendering scope. Only textures you create yourself with `CTX::createTexture` need it set by hand, usually for MSAA targets.
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

// Anisotropic sampler
mythril::Sampler aniso = ctx->createSampler({
    .mipMap         = mythril::SamplerMipMap::Linear,
    .anisotropic    = true,
    .maxAnisotropic = 8,
    .debugName      = "Anisotropic Sampler",
});
```

---

## Shaders

Shaders are written in [Slang](https://shader-slang.com/) and compiled to SPIR-V at build time by `mythril_reflect_shaders` in CMake. For each shader it writes `<shader>.slang.spv` into `SPIRV_DIR`, and a header `<shader>.shadergen.h` with the push constant structs and the file name in `kSpirvFile`. Create a shader from that file with `CTX::createShader()`.

A struct declared in an imported module goes in `<module>.shadergen.h`, and each shader header that uses it includes that header. Add each such module to `MODULES`, so CMake knows about its header. If a module is missing from `MODULES`, the build fails with an error that names it.

```cmake
mythril_reflect_shaders(my_app SOURCES shaders/PBR.slang SPIRV_DIR "$<TARGET_FILE_DIR:my_app>/shaders"
        TYPES_HEADER shader_types.h TYPES_NAMESPACE my_app::gpu MATRIX_LAYOUT COLUMN)
```

The generated structs use C++ types for Slang's vectors and matrices (`float4`, `float4x4` and so on). These options tell the tool which types to use:

| Option | Value |
|---|---|
| `TYPES_HEADER` | The header that the generated headers include for those types. `mythril/ShaderTypes.h` (DEFAULT) has plain structs with no math. |
| `TYPES_NAMESPACE` | The namespace that holds the types, `mythril::shader` (DEFAULT). |
| `MATRIX_LAYOUT` | `ROW` (DEFAULT) or `COLUMN`, the matrix layout of the Slang session. Set `COLUMN` for glm, so a `glm::mat4` goes into a `float4x4` field unchanged. |
| `NAMESPACE` | The namespace of the generated code, `shaders` (DEFAULT). |

To use glm, write a header with aliases such as `using float4x4 = glm::mat4;` in your namespace, as `samples/shader_types.h` does. The pass example [below](#passes) needs these options, because it puts a `glm::mat4` in `.mvp`.

A pointer field (`T*` in Slang) is a `mythril::DevicePtr<T>` in C++. `CTX::gpuAddress()` returns a `mythril::DeviceAddress`, which converts to any `DevicePtr<T>` and to `AccelerationStructureSlot`, so you can write `.vertices = ctx->gpuAddress(vertexBuffer)` without the pointee type.

The header also holds values from the shader, in namespace `shaders::<shader>`. Use them so the C++ and Slang code cannot disagree:
- `kSpirvFile`: the `.spv` file name.
- `kThreadGroupSize`: the `[numthreads]` of a compute shader, as `uint32_t[3]`.
- Each module scope `static const int` or `static const uint`, with its Slang name. A value that Slang computes from other constants is written as the result.

```cpp
cmd.cmdDispatchThreadGroup({(count + shaders::Cull::kThreadGroupSize[0] - 1) / shaders::Cull::kThreadGroupSize[0], 1, 1});
```

```cpp
#include "PBR.shadergen.h"

mythril::Shader defaultShader = ctx->createShader({
    .path      = kShaderDir / shaders::PBR::kSpirvFile,
    .debugName = "PBR Shader",
});
```

A single `.slang` file can contain both vertex and fragment entry points, there is no requirement to split them into separate files or separate `Shader` objects.
You can also inspect the information of a shader at runtime thanks to our reflection system, do so with `CTX::inspectShader`, you can see it being used in sample_06.

You can incorporate `CTX::reloadShaderEXT` if you want hot reloading functionality, however it's very experimental.

**Good to Knows**:
- Keep the `Shader` alive for as long as any pipeline that uses it exists. Destroying the shader while a pipeline still references it is undefined behavior. *(subject to change in the future)*
- `createShader` stops the program if the `.spv` file is missing or cannot be read, so ship the `SPIRV_DIR` files with your executable.
- When `SPIRV_DIR` is a plain path, the build writes a deleted `.spv` file again. CMake cannot track files in a path that has a target generator expression, such as `$<TARGET_FILE_DIR:my_app>`. With that kind of path, change a `.slang` file to write the `.spv` files again.
- `reloadShaderEXT` works only in a debug build of mythril. A release build always returns false.

---

## Pipelines

Pipelines bind shaders and fixed-function state. Mythril supports graphics, compute, and raytracing pipelines as of now.

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
// equivalent
.vertexShader = {defaultShader, "vertMain"},
```

### Compute Pipeline

```cpp
mythril::ComputePipeline compute = ctx->createComputePipeline({
    .shader    = computeShader.handle(),
    .debugName = "Particle Update",
});
```

### RayTracing Pipeline

```cpp
mythril::RayTracingPipeline mainPipeline = ctx->createRayTracingPipeline({
    .rayGenShaders = {
        { raytraceShader, "raygenMain" }
    },
    .missShaders = {
        { raytraceShader, "missMain" },
        { raytraceShader, "missShadowMain" }
    },
    .closestHitShaders = {
        { raytraceShader, "closestHitMain" }
    },
    .debugName = "Main Pipeline"
});
```

### Specialization Constants

All pipeline types support up to 16 specialization constants, which let you bake values into the SPIR-V at pipeline creation time without recompiling the shader:

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
- An entry's `size` must match the constant in the shader (a `bool` takes a `VkBool32`). In debug builds, binding a pipeline with a wrong size, or with a name its shader does not have, asserts. An id the shader does not have is ignored.
- Multiple pipelines can share a single `Shader`, as there is no need to duplicate shader objects per pipeline.

### Acceleration Structures

Acceleration structures are a required part of the raytracing pipeline, and can be quite complicated to create correctly, definitely refer to sample_08 since they will likely be rewritten to be more user-friendly.

Heres an example of a TLAS:

```cpp
mythril::AccelerationStructure tlas = ctx->createAccelerationStructure({
    .type = mythril::AccelerationStructureSpec::Type::TLAS,
    .geometryType = mythril::AccelerationStructureSpec::GeometryType::Instances,
    .instancesBuffer = instanceBuf.handle(),
    .buildRange = { .primitiveCount = static_cast<uint32_t>(blases.size()) },
    .buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .debugName = "TLAS Accel"
});
```
**Good to Knows**:
- They are quite experimental so if you run into an issue please let me know lol
- Acceleration structures are not bindless descriptors. A `DescriptorHandle<RaytracingAccelerationStructure>` push constant field is an `AccelerationStructureSlot` in the generated struct. Set it to `ctx->gpuAddress(tlas)`, the same way buffer pointers are passed, and keep the `AccelerationStructure` alive past the frames in flight.

To build or refit an acceleration structure every frame, give it to the graph with `graph.importAccelerationStructure(structure)`, once, next to your other imports. This returns a `GraphAccelerationStructure`, and the storage and scratch buffers of the structure become graph buffers. Then build it in a transfer pass:

```cpp
const mythril::GraphAccelerationStructure rgBlas = graph.importAccelerationStructure(blas);
const mythril::GraphAccelerationStructure rgTlas = graph.importAccelerationStructure(tlas);

// each frame
mythril::GraphAccelerationStructure blasBuilt = rgBlas;
mythril::GraphAccelerationStructure tlasBuilt = rgTlas;
graph.addTransferPass("build acceleration structures", [&](mythril::TransferBuilder& b) {
    blasBuilt = b.refit(blasBuilt, std::array{rgVertices, rgIndices});
    tlasBuilt = b.build(tlasBuilt, std::array{rgInstances}, std::array{blasBuilt});
});
graph.addRayTracingPass<TracePass>("trace", [&](mythril::RayTracingPassBuilder& b, TracePass& d) {
    b.dependency(tlasBuilt);
    // ...
}, /* execute */);
```

- `build(structure, inputs, instanced)` and `refit(...)` return the next version of the structure, like any other write. `inputs` are all the buffers the structure was created from. For a TLAS, `instanced` names each BLAS that its instances point to, and a pass that reads the TLAS also reads those BLASes.
- `refit` keeps the topology and only moves the geometry. The structure needs `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` in its `buildFlags`.
- A pass that traces rays reads the structure with `b.dependency(structure)`.
- A pass that builds or refits never runs on the transfer queue, because the transfer queue cannot build acceleration structures. With `async()` it runs on async compute, and otherwise on the main queue.

# RenderGraph

The rendergraph is Mythril's special sauce, allowing a developer to simply write each unit of GPU work as a 'pass', and *magically* Mythril will compile everything to the most optimal ordering of Vulkan commands.
Passes can be delcared in any order, since all that matters is the relationship between them.
The samples are your friend here since they show various use cases of a pass. 
We use the words framegraph and rendergraph interchangably in the docs.

## Resources

Create one `RenderGraph` and tell it which resources it works with, once, outside your frame loop. Passes name resources by `GraphTexture` and `GraphBuffer` ids, not by `Texture` and `Buffer` objects. These ids survive `reset()`.

- `graph.createTexture(spec)` makes a texture the graph owns. You only give it what the graph can't infer (size, format, samples, mips, layers), usage and storage come from the passes that use it. `size` is either fixed `Dimensions` or a `RelativeSize`, which follows the swapchain by default or an extent from `graph.createExtent()` that you resize with `graph.setExtent()`.
- `graph.importTexture(texture)` hands the graph a texture you created and keep alive. Importing the same texture twice returns the same id.
- `graph.importBuffer(buffer)` does the same for buffers. Every buffer a pass touches must be imported, so the graph can place its barriers.
- `graph.backBuffer()` is the swapchain image, returns whichever one is current when the graph records.

```cpp
mythril::RenderGraph graph;
const mythril::GraphTexture rgColor = graph.createTexture({
    .size = mythril::RelativeSize{},
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .debugName = "Color Texture"
});
const mythril::GraphBuffer rgVertices = graph.importBuffer(vertexBuffer);
```

## The Frame Loop

The graph is rebuilt every frame. All the CPU work of figuring out barriers, ordering and queues happens in `compile()`, so building passes is cheap.

```cpp
while (!quit) {
    graph.reset();     // drops last frame's passes but keeps resources
    buildGraph();      // your add*Pass calls
    graph.compile(*ctx);
    graph.execute(*ctx);
}
```

`execute()` records, submits and presents the frame. The swapchain image is only acquired and presented when a pass that survives compilation uses `backBuffer()`, so a headless graph just works.

## Passes

Every pass is two callbacks and an optional `Data` struct:
- **setup** runs immediately inside `add*Pass`. It declares what the pass reads and writes through the builder, and fills `Data` with anything execute needs.
- **execute** runs later when the graph records. It must not capture anything and takes `(const Data&, XxxCmd&)`, everything it needs goes through `Data`.

The example draws with sample_02's `BasicRed.slang`, so `shaders::PushConstant` and `shaders::Vertex` come from the generated `BasicRed.shadergen.h`. sample_02 uses the glm options from [Shaders](#shaders).

```cpp
struct MainPass {
    mythril::GraphicsPipelineHandle pipeline;
    mythril::BufferHandle indexBuffer;
    uint32_t indexCount = 0;
    glm::mat4 mvp{};
    mythril::DeviceAddress vertices;
};

mythril::GraphTexture color = rgColor;
graph.addGraphicsPass<MainPass>("main", [&](mythril::GraphicsPassBuilder& b, MainPass& d) {
    b.attachment(color, {
        .clearValue = mythril::ClearValue::color(0.2f, 0.2f, 0.2f, 1.f),
        .loadOp = mythril::LoadOp::CLEAR,
        .storeOp = mythril::StoreOp::STORE
    });
    b.dependency(rgVertices, mythril::BufferAccess::ShaderRead);
    b.dependency(rgIndices, mythril::BufferAccess::IndexRead);

    d.pipeline = pipeline.handle();
    d.indexBuffer = indexBuffer.handle();
    d.indexCount = indexCount;
    d.mvp = projection * view * model;
    d.vertices = ctx->gpuAddress(vertexBuffer);
}, [](const MainPass& d, mythril::GraphicsCmd& cmd) {
    cmd.cmdBindGraphicsPipeline(d.pipeline);
    const shaders::PushConstant push = {
        .mvp = d.mvp,
        .vertexBufferAddress = d.vertices
    };
    cmd.cmdPushConstants(push);
    cmd.cmdBindIndexBuffer(d.indexBuffer);
    cmd.cmdDrawIndexed(d.indexCount);
});
```

If a pass needs no data, use `mythril::NoPassData` for both callbacks.

The pass types are:
- `addGraphicsPass`: used alongside a `GraphicsPipeline`, execute gets a `GraphicsCmd`.
- `addComputePass`: used alongside a `ComputePipeline`, execute gets a `ComputeCmd`.
- `addRayTracingPass`: used alongside a `RayTracingPipeline`, execute gets a `RayTracingCmd`.
- `addTransferPass`: see [below](#add-transfer-pass).

Each `XxxCmd` only exposes the commands that are valid for its pass, so you can't for example draw inside a compute pass. They all share `texture(GraphTexture)` to get the real handle and dimensions of a graph texture, plus `vk()`, `vkImage()` and `vkBuffer()` if you need raw Vulkan objects (experimental).

### Versions

A `GraphTexture` or `GraphBuffer` is one *version* of a resource. Any write (an attachment, a write dependency, a transfer op) produces the next version, and later passes should use that version. This is how the graph knows the order your passes depend on each other. Reads return the version they name.

`attachment(texture, options)` takes the texture by reference and updates it in place to the version the pass writes, so later passes can use `color` directly. `attachment(texture, resolve, options)` does the same for an MSAA texture and its resolve target, and updates both. The texture must be a non-const `GraphTexture`, copy a const handle into a local first.

Dependencies take a `TextureAccess` (`Sampled`, `StorageRead`, `StorageWrite`, `StorageReadWrite`, `TransferRead`, `TransferWrite`) or a `BufferAccess` (`ShaderRead`, `ShaderWrite`, `ShaderReadWrite`, `IndexRead`, `IndirectRead`, `TransferRead`, `TransferWrite`). 
You can also pass a span of textures to declare many at once, a mutable span is written in place and a const span will only allows reads.

### Pass Options

These can called on the builder during the setup phase:
- `async()` (compute, ray tracing, transfer): run on the async compute or transfer queue when the device has one, otherwise it falls back to the main queue. Ownership transfers between queues are handled for you.
- `executeCmdsInOrder()` (compute, ray tracing): dispatches inside a pass are unordered by default, use this when one reads what a previous one wrote (ie: a mip chain). It costs a barrier between each command.
- `sideEffect()` (graphics, compute, ray tracing): passes whose output nothing reads are culled, use this to keep one alive that writes something the graph can't see (queries, bindless writes, debug printf).
- `multiview(layerCount, viewMask)` (graphics).

<a id="add-transfer-pass"></a>
### Transfer Passes

`addTransferPass` is for per-frame work that doesn't need a pipeline. You should use this instead of `CTX::upload` for anything per-frame, since the graph can place the work where it costs the least. Its setup only takes a `TransferBuilder`:

```cpp
graph.addTransferPass("per_frame_uploads", [&](mythril::TransferBuilder& b) {
    rgFrameData = b.update(rgFrameData, frameData);
});
graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
    b.blit(color, graph.backBuffer());
});
```

- `copy(src, dst)` / `blit(src, dst)`: texture to texture. `copy` also works buffer to buffer.
- `update(buffer, data, offset)`: up to 64kb, size and offset must be multiples of 4.
- `upload(buffer, data, offset)`: no size limit, goes through staging.
- `upload(texture, data)`: one whole mip, tightly packed.
- `readback(texture, buffer)`: one mip, tightly packed at the start of the buffer.
- `generateMipmaps(texture)`: fills every mip below the top one with blits.
- `async()`: run on the transfer queue, then async compute, then main. Blits need the main queue, so `blit` and `generateMipmaps` assert in an async pass.

`update` and `upload` accept a span, any contiguous range or a single trivially copyable value, and the bytes are copied into the graph during setup so your data doesn't need to outlive the call. 
There is also a three argument `addTransferPass(name, setup, execute)` whose execute gets a `TransferCmd` and runs after the pass's ops, for raw transfer commands through `vk()`.

## Options & Debugging

`graph.setOptions()` turns graph features on and off, which is mostly useful for debugging:
- `cull` (default on): drop passes whose output nothing uses.
- `merge` (default on): merge graphics passes that can share one rendering scope.
- `async` (default on): allow `async()` passes onto other queues.
- `fullBarriers` (default off): an `ALL_COMMANDS` barrier between every pass.
- `declaredOrder` (default off): schedule in the order you added passes and assert when that order breaks a dependency.

After compiling you can inspect what the graph did with `graph.schedule()` (the passes in record order, then the culled ones), or `graph.dump()` which returns a graphviz graph of passes, versions, barriers and queues. `execute()` also takes an optional timestamps `QueryPool`, which gets a begin/end pair per merge group.

## External Sync

If a resource is also used by work outside the graph (OpenXR, video, CUDA, another process), pass an `ExternalAccess` when importing it. It holds the semaphores to wait on and signal, the layouts on entry and exit, and the queue family that owns it outside the frame. `reset()` clears it, so call the import again with it every frame.

Both semaphores are timeline semaphores. The wait happens before the first pass that uses the resource, and the signal after the last one, so the rest of the frame doesn't stall on the outside work.

As an example, say a video decoder on its own queue family writes each frame into `videoFrame`, and signals `decodeTimeline` when it is done. The graph samples it, then hands it back so the decoder can write the next frame:

```cpp
mythril::Texture videoFrame = ctx->createTexture({ /* ... */ });
VkSemaphore decodeTimeline = /* VkSemaphoreTypeCreateInfo with VK_SEMAPHORE_TYPE_TIMELINE */;
VkSemaphore renderTimeline = /* same */;
uint64_t frame = 0;

while (!quit) {
    ++frame;
    // decoder: waits on renderTimeline >= frame - 1, writes videoFrame, then signals decodeTimeline = frame

    const mythril::ExternalAccess videoAccess = {
        .waitSemaphore = decodeTimeline,
        .waitValue = frame,
        .signalSemaphore = renderTimeline,
        .signalValue = frame,
        .entryLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,  // the layout the decoder leaves it in
        .exitLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   // the layout the decoder expects back
        .ownerFamily = decoderQueueFamily,
    };

    graph.reset();
    const mythril::GraphTexture rgVideo = graph.importTexture(videoFrame, videoAccess);

    graph.addGraphicsPass<VideoPass>("draw_video", [&](mythril::GraphicsPassBuilder& b, VideoPass& d) {
        mythril::GraphTexture backBuffer = graph.backBuffer();
        b.attachment(backBuffer, { .loadOp = mythril::LoadOp::CLEAR, .storeOp = mythril::StoreOp::STORE });
        b.dependency(rgVideo, mythril::TextureAccess::Sampled);
        d.video = rgVideo;
        // ...
    }, [](const VideoPass& d, mythril::GraphicsCmd& cmd) {
        const mythril::TextureHandle video = cmd.texture(d.video).handle;
        // push video's index and draw a fullscreen quad
    });

    graph.compile(*ctx);
    graph.execute(*ctx);
}
```

Because `ownerFamily` is set and differs from the graph's queue, the graph also records the queue family acquire on entry and the release on exit for you. Leave `ownerFamily` as `VK_QUEUE_FAMILY_IGNORED` when the outside work runs on the same family, or when the resource is concurrent. An `entryLayout` of `UNDEFINED` tells the graph the contents don't matter, which skips the acquire.

# Cleanup

There is no manual cleanup necessary, as `Mythril`'s Vulkan objects will perform the necessary removal logic when their destructor is called. The same goes for `mythril::CTX` which will perform cleanup for all previously created objects if their destructors have not already been called.

What you do have to do is keep the windowing system/context alive until AFTER `mythril::CTX` is destroyed, as destroying your window, i.e. `SDL_Window*` too early will result in validation errors. This is why all the samples scope `mythril::CTX` but not the created window.