//
// Created by Hayden Rivas on 7/1/26.
//

#include <mythril/CTX.h>
#include <mythril/CTXBuilder.h>

#include <SDL3/SDL_vulkan.h>

#include "raytrace.shadergen.h"
#include "Wind.shadergen.h"
#include "Leaves.shadergen.h"
#include "InstanceBounds.shadergen.h"
#include "BoundsLines.shadergen.h"
#include "../SDL3Usage.h"
#include "../SampleUtils.h"

#include "../FlyCamera.h"
#include "Scene.h"

// must match the kDebug* constants in raytrace.slang
static constexpr const char* kDebugModeNames[] = {
    "Lit", "Normals", "Hit Distance", "Primitive ID", "Barycentrics", "Shadow Rays", "Ray Direction", "Bounds"
};
static constexpr uint32_t kNumDebugModes = std::size(kDebugModeNames);
static_assert(kNumDebugModes == shaders::raytrace::kDebugBounds + 1);
static constexpr uint32_t kUnionBoxCount = 1;

static void ShowDebugModeInTitle(SDL_Window* window, const uint32_t mode) {
    char title[128];
    snprintf(title, sizeof(title), "08_RayTracing - [%u] %s (keys 0-%u)", mode, kDebugModeNames[mode], kNumDebugModes - 1);
    SDL_SetWindowTitle(window, title);
}

struct WindPass {
    mythril::ComputePipelineHandle pipeline;
    std::array<shaders::WindPushConstant, kTreeCount> pushes;
};
struct LeavesPass {
    mythril::ComputePipelineHandle pipeline;
    shaders::LeavesPushConstant push;
};
struct BoundsPass {
    mythril::ComputePipelineHandle pipeline;
    shaders::BoundsPushConstant push;
};
struct TracePass {
    mythril::RayTracingPipelineHandle pipeline;
    mythril::GraphTexture output;
    mythril::DeviceAddress frame;
    mythril::DeviceAddress geometries;
    mythril::DeviceAddress tlas;
    mythril::GraphTexture distance;
    uint32_t debugMode = 0;
};
struct BoundsLinesPass {
    mythril::GraphicsPipelineHandle pipeline;
    mythril::GraphTexture distance;
    shaders::BoundsLinesPushConstant push;
};

int main() {
    static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
    SDL_Window* sdlWindow = BuildSDLWindow(true);
    const auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
    static const std::vector<const char*> vulkan_extensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .accelerationStructure = VK_TRUE,
        .accelerationStructureCaptureReplay = VK_FALSE,
        .accelerationStructureIndirectBuild = VK_FALSE,
        .accelerationStructureHostCommands = VK_FALSE,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &accelerationStructureFeatures,
        .rayTracingPipeline = VK_TRUE,
        .rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE,
        .rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE,
        .rayTracingPipelineTraceRaysIndirect = VK_TRUE,
        .rayTraversalPrimitiveCulling = VK_FALSE,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &rayTracingPipelineFeatures,
        .rayQuery = VK_TRUE,
    };
    {
        Uint32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        auto ctx =
            mythril::CTXBuilder{}
                .set_vulkan_cfg({
                    .app_name = "Cool App Name",
                    .engine_name = "Cool Engine Name",
                    .app_version = { 0, 0, 1 },
                    .engine_version = { 0, 0, 1 },
                    .enableValidation = true,
                    .deviceExtensionNames = vulkan_extensions,
                    .additionalDeviceFeatures = {
                        .shaderInt64 = VK_TRUE,
                    },
                    .deviceExtensionFeatureChain = &rayQueryFeatures,
                })
                .set_window_surface({
                    .instanceExtensions = std::span(sdlExtensions, sdlExtensionCount),
                    .createFunc = [sdlWindow](VkInstance instance) {
                        VkSurfaceKHR surface;
                        SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
                        return surface;
                    },
                    .destroyFunc = [](VkInstance instance, VkSurfaceKHR surface) {
                        SDL_Vulkan_DestroySurface(instance, surface, nullptr);
                    },
                })
                .with_default_swapchain({
                    .width = initialWindowSize.width,
                    .height = initialWindowSize.height,
                    .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
                })
                .build();

        mythril::Shader raytraceShader = ctx->createShader({
            .path = kDataDir / shaders::raytrace::kSpirvFile,
            .debugName = "Raytrace Shader",
        });
        mythril::RayTracingPipeline mainPipeline = ctx->createRayTracingPipeline({
            .rayGenShaders = {
                { raytraceShader, "raygenMain" } },
            .missShaders = { { raytraceShader, "missMain" }, { raytraceShader, "missShadowMain" } },
            .closestHitShaders = { { raytraceShader, "closestHitMain" } },
            .debugName = "Main Pipeline",
        });
        mythril::Shader windShader = ctx->createShader({
            .path = kDataDir / shaders::Wind::kSpirvFile,
            .debugName = "Wind Shader",
        });
        mythril::ComputePipeline windPipeline = ctx->createComputePipeline({
            .shader = windShader.handle(),
            .debugName = "Wind Pipeline",
        });
        mythril::Shader leavesShader = ctx->createShader({
            .path = kDataDir / shaders::Leaves::kSpirvFile,
            .debugName = "Leaves Shader",
        });
        mythril::ComputePipeline leavesPipeline = ctx->createComputePipeline({
            .shader = leavesShader.handle(),
            .debugName = "Leaves Pipeline",
        });
        mythril::Shader boundsShader = ctx->createShader({
            .path = kDataDir / shaders::InstanceBounds::kSpirvFile,
            .debugName = "Bounds Shader",
        });
        mythril::ComputePipeline boundsPipeline = ctx->createComputePipeline({
            .shader = boundsShader.handle(),
            .debugName = "Bounds Pipeline",
        });
        mythril::Shader boundsLinesShader = ctx->createShader({
            .path = kDataDir / shaders::BoundsLines::kSpirvFile,
            .debugName = "Bounds Lines Shader",
        });
        mythril::GraphicsPipeline boundsLinesPipeline = ctx->createGraphicsPipeline({
            .vertexShader = { boundsLinesShader },
            .fragmentShader = { boundsLinesShader },
            .blend = mythril::BlendingMode::ALPHA_BLEND,
            .debugName = "Bounds Lines Pipeline",
        });

        mythril::Buffer frameBuffer = ctx->createBuffer({
            .size = sizeof(shaders::PerFrame),
            .usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
            .storage = mythril::StorageType::Device,
            .debugName = "Frame Uniforms",
        });
        const Scene scene = createScene(*ctx);

        mythril::RenderGraph graph;
        const mythril::GraphTexture rgTracedShadowOutput = graph.createTexture({
            .size = mythril::RelativeSize{},
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .debugName = "Raytraced output",
        });
        const mythril::GraphTexture rgSceneDistance = graph.createTexture({
            .size = mythril::RelativeSize{},
            .format = VK_FORMAT_R32_SFLOAT,
            .debugName = "Scene distance",
        });
        const mythril::GraphBuffer rgFrameData = graph.importBuffer(frameBuffer);
        const SceneGraphResources rg = importScene(graph, scene);

        FlyCamera camera(sdlWindow, { 0.f, 1.5f, 12.f }, {});
        const float aspectRatio = static_cast<float>(initialWindowSize.width) / static_cast<float>(initialWindowSize.height);
        const glm::vec2 kWindDirection = glm::normalize(glm::vec2(1.f, 0.35f));
        float windTime = 0.f;
        bool windPaused = false;
        uint32_t debugMode = 0;
        ShowDebugModeInTitle(sdlWindow, debugMode);

        const auto buildGraph = [&](const shaders::PerFrame& frameData, const glm::mat4& viewProjection) {
            mythril::GraphBuffer frame = rgFrameData;
            graph.addTransferPass("per_frame_uploads", [&](mythril::TransferBuilder& b) {
                frame = b.update(frame, frameData);
            });

            std::array<mythril::GraphBuffer, kTreeCount> bent = rg.treeBent;
            graph.addComputePass<WindPass>("wind", [&](mythril::ComputePassBuilder& b, WindPass& d) {
                d.pipeline = windPipeline.handle();
                for (uint32_t i = 0; i < kTreeCount; i++) {
                    const Tree& tree = scene.trees[i];
                    b.dependency(rg.treeRest[i], mythril::BufferAccess::ShaderRead);
                    bent[i] = b.dependency(bent[i], mythril::BufferAccess::ShaderWrite);
                    d.pushes[i] = {
                        .rest = ctx->gpuAddress(tree.rest),
                        .bent = ctx->gpuAddress(tree.bent.vertices),
                        .root = tree.root,
                        .height = tree.height,
                        .windDirection = kWindDirection,
                        .time = windTime,
                        .strength = 0.9f,
                        .vertexCount = tree.bent.vertexCount,
                        .phase = tree.phase,
                    };
                }
            },
                [](const WindPass& d, mythril::ComputeCmd& cmd) {
                    cmd.cmdBindComputePipeline(d.pipeline);
                    for (const shaders::WindPushConstant& push: d.pushes) {
                        cmd.cmdPushConstants(push);
                        cmd.cmdDispatchThreadGroup(sample::GroupsFor({ push.vertexCount }, shaders::Wind::kThreadGroupSize));
                    }
                });

            mythril::GraphBuffer instances = rg.instances;
            graph.addComputePass<LeavesPass>("blow leaves", [&](mythril::ComputePassBuilder& b, LeavesPass& d) {
                instances = b.dependency(instances, mythril::BufferAccess::ShaderWrite);
                d.pipeline = leavesPipeline.handle();
                d.push = {
                    .leaves = ctx->gpuAddress(scene.instances, kFirstLeafInstance * sizeof(shaders::Instance)),
                    .leafBlas = ctx->gpuAddress(scene.leaf.blas),
                    .crown0 = scene.trees[0].crown(),
                    .crownRadius = 1.1f,
                    .crown1 = scene.trees[1].crown(),
                    .floorHeight = kFloorHeight,
                    .windDirection = kWindDirection,
                    .time = windTime,
                    .leafCount = kLeafCount,
                };
            },
                [](const LeavesPass& d, mythril::ComputeCmd& cmd) {
                    cmd.cmdBindComputePipeline(d.pipeline);
                    cmd.cmdPushConstants(d.push);
                    cmd.cmdDispatchThreadGroup(sample::GroupsFor({ d.push.leafCount }, shaders::Leaves::kThreadGroupSize));
                });

            std::array<mythril::GraphAccelerationStructure, kTreeCount> treeBlas = rg.treeBlas;
            graph.addTransferPass("refit trees", [&](mythril::TransferBuilder& b) {
                for (uint32_t i = 0; i < kTreeCount; i++)
                    treeBlas[i] = b.refit(treeBlas[i], std::array{ bent[i], rg.treeIndices[i] });
            });
            mythril::GraphAccelerationStructure tlasBuilt = rg.tlas;
            graph.addTransferPass("build tlas", [&](mythril::TransferBuilder& b) {
                std::array<mythril::GraphAccelerationStructure, 2 + kTreeCount> blases = { rg.groundBlas, rg.leafBlas };
                std::ranges::copy(treeBlas, blases.begin() + 2);
                tlasBuilt = b.build(tlasBuilt, std::array{ instances }, blases);
            });

            mythril::GraphBuffer bounds = rg.bounds;
            const bool showBounds = debugMode == shaders::raytrace::kDebugBounds;
            if (showBounds) {
                graph.addComputePass<BoundsPass>("instance bounds", [&](mythril::ComputePassBuilder& b, BoundsPass& d) {
                    b.dependency(instances, mythril::BufferAccess::ShaderRead);
                    b.dependency(rg.geometries, mythril::BufferAccess::ShaderRead);
                    for (const mythril::GraphBuffer buffer: GeometryBuffers(rg, bent))
                        b.dependency(buffer, mythril::BufferAccess::ShaderRead);
                    bounds = b.dependency(bounds, mythril::BufferAccess::ShaderWrite);
                    d.pipeline = boundsPipeline.handle();
                    d.push = {
                        .instances = ctx->gpuAddress(scene.instances),
                        .geometries = ctx->gpuAddress(scene.geometries),
                        .bounds = ctx->gpuAddress(scene.bounds),
                        .instanceCount = scene.instanceCount,
                    };
                },
                    [](const BoundsPass& d, mythril::ComputeCmd& cmd) {
                        cmd.cmdBindComputePipeline(d.pipeline);
                        cmd.cmdPushConstants(d.push);
                        cmd.cmdDispatchThreadGroup({ d.push.instanceCount, 1, 1 });
                    });
            }

            mythril::GraphTexture traced = rgTracedShadowOutput;
            mythril::GraphTexture distance = rgSceneDistance;
            graph.addRayTracingPass<TracePass>("trace shadows", [&](mythril::RayTracingPassBuilder& b, TracePass& d) {
                traced = b.dependency(traced, mythril::TextureAccess::StorageWrite);
                b.dependency(tlasBuilt);
                b.dependency(frame, mythril::BufferAccess::ShaderRead);
                b.dependency(rg.geometries, mythril::BufferAccess::ShaderRead);
                for (const mythril::GraphBuffer buffer: GeometryBuffers(rg, bent))
                    b.dependency(buffer, mythril::BufferAccess::ShaderRead);
                d.pipeline = mainPipeline.handle();
                d.output = traced;
                d.frame = ctx->gpuAddress(frameBuffer);
                d.geometries = ctx->gpuAddress(scene.geometries);
                d.tlas = ctx->gpuAddress(scene.tlas);
                if (showBounds)
                    d.distance = distance = b.dependency(distance, mythril::TextureAccess::StorageWrite);
                d.debugMode = debugMode;
            },
                [](const TracePass& d, mythril::RayTracingCmd& cmd) {
                    cmd.cmdBindRayTracingPipeline(d.pipeline);
                    const mythril::ResolvedTexture tracedOutput = cmd.texture(d.output);
                    const shaders::ShadowPushConstant push = {
                        .frame = d.frame,
                        // points toward the light, closestHitMain dots it with the surface normal
                        .lightDir = glm::vec4(1.5, 2.0, 1.0, 0),
                        .geometries = d.geometries,
                        .outTexture = tracedOutput,
                        .tlas = d.tlas,
                        .enableShadows = static_cast<uint32_t>(true),
                        .debugMode = d.debugMode,
                        .boundsHitDistance = d.distance.valid() ? cmd.texture(d.distance) : mythril::ResolvedTexture{},
                    };
                    cmd.cmdPushConstants(push);
                    // must match the output texture, out of bounds image stores are not discarded without robustness
                    cmd.cmdTraceRays({ tracedOutput.dimensions.width, tracedOutput.dimensions.height, 1 });
                });
            if (showBounds) {
                graph.addGraphicsPass<BoundsLinesPass>("draw bounds", [&](mythril::GraphicsPassBuilder& b, BoundsLinesPass& d) {
                    traced = b.attachment({
                                              .texDesc = traced,
                                              .loadOp = mythril::LoadOp::LOAD,
                                              .storeOp = mythril::StoreOp::STORE,
                                          })
                                 .texture;
                    b.dependency(bounds, mythril::BufferAccess::ShaderRead);
                    d.distance = b.dependency(distance, mythril::TextureAccess::Sampled);
                    d.pipeline = boundsLinesPipeline.handle();
                    d.push = {
                        .viewProjection = viewProjection,
                        .cameraPosition = camera.position(),
                        .boundsCount = scene.instanceCount,
                        .bounds = ctx->gpuAddress(scene.bounds),
                        .halfWidth = 1.5f,
                    };
                },
                    [](const BoundsLinesPass& d, mythril::GraphicsCmd& cmd) {
                        const mythril::ResolvedTexture distance = cmd.texture(d.distance);
                        shaders::BoundsLinesPushConstant push = d.push;
                        push.sceneDistance = distance;
                        push.viewportSize = glm::vec2(distance.dimensions.width, distance.dimensions.height);
                        cmd.cmdBindGraphicsPipeline(d.pipeline);
                        cmd.cmdPushConstants(push);
                        cmd.cmdDraw(shaders::BoundsLines::kVerticesPerBox * (push.boundsCount + kUnionBoxCount));
                    });
            }
            graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
                b.blit(traced, graph.backBuffer());
            });
        };

        auto lastTime = std::chrono::steady_clock::now();
        bool quit = false;
        while (!quit) {
            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - lastTime).count();
            lastTime = now;
            if (!windPaused)
                windTime += deltaTime;

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                camera.handleEvent(e);
                if (e.type == SDL_EVENT_QUIT) quit = true;
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_Q) quit = true;
                    if (e.key.key == SDLK_P) windPaused = !windPaused;
                    if (e.key.key >= SDLK_0 && e.key.key < SDLK_0 + kNumDebugModes)
                        ShowDebugModeInTitle(sdlWindow, debugMode = e.key.key - SDLK_0);
                }
            }
            camera.update(deltaTime);

            const glm::mat4 view = camera.view();
            const glm::mat4 projection = camera.projection(aspectRatio);
            const shaders::PerFrame frameData = {
                .viewInverse = glm::inverse(view),
                .projInverse = glm::inverse(projection),
            };

            graph.reset();
            buildGraph(frameData, projection * view);
            graph.compile(*ctx);
            graph.execute(*ctx);
        }
    }
    DestroySDLWindow(sdlWindow);
    return 0;
}
