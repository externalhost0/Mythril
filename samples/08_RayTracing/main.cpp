//
// Created by Hayden Rivas on 7/1/26.
//

#include <mythril/CTX.h>
#include "../SampleUtils.h"
#include <mythril/CTXBuilder.h>

#include <SDL3/SDL_vulkan.h>
#include <glm/vec3.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "../SDL3Usage.h"
#include "glm/detail/type_mat4x4.hpp"

#include "GPUStructs.h"
#include "Meshes.h"

struct Camera {
	glm::vec3 position;
	glm::vec3 forwardVector;
	glm::vec3 upVector;
	float aspectRatio;
	float fov;
	float nearPlane;
	float farPlane;
};


const std::vector<Vertex> cubeVertices = {
	// front face
	{{-1.f, -1.f, -1.f}}, // A 0
	{{ 1.f, -1.f, -1.f}}, // B 1
	{{ 1.f,  1.f, -1.f}}, // C 2
	{{-1.f,  1.f, -1.f}}, // D 3

	// back face
	{{-1.f, -1.f, 1.f}}, // E 4
	{{ 1.f, -1.f, 1.f}}, // F 5
	{{ 1.f,  1.f, 1.f}}, // G 6
	{{-1.f,  1.f, 1.f}},  // H 7

	// left face
	{{-1.f,  1.f, -1.f}}, // D 8
	{{-1.f, -1.f, -1.f}}, // A 9
	{{-1.f, -1.f,  1.f}}, // E 10
	{{-1.f,  1.f,  1.f}}, // H 11

	// right face
	{{1.f, -1.f, -1.f}}, // B 12
	{{1.f,  1.f, -1.f}}, // C 13
	{{1.f,  1.f,  1.f}}, // G 14
	{{1.f, -1.f,  1.f}}, // F 15

	// bottom face
	{{-1.f, -1.f, -1.f}}, // A 16
	{{ 1.f, -1.f, -1.f}}, // B 17
	{{ 1.f, -1.f,  1.f}}, // F 18
	{{-1.f, -1.f,  1.f}}, // E 19

	// top face
	{{ 1.f, 1.f, -1.f}}, // C 20
	{{-1.f, 1.f, -1.f}}, // D 21
	{{-1.f, 1.f,  1.f}}, // H 22
	{{ 1.f, 1.f,  1.f}}, // G 23
};
const std::vector<uint32_t> cubeIndices = {
	// front and back
	0, 3, 2,
	2, 1, 0,
	4, 5, 6,
	6, 7 ,4,
	// left and right
	11, 8, 9,
	9, 10, 11,
	12, 13, 14,
	14, 15, 12,
	// bottom and top
	16, 17, 18,
	18, 19, 16,
	20, 21, 22,
	22, 23, 20
};

Mesh buildSceneMesh(float floorHalfExtent, float floorHeight = -1.f) {
	Mesh mesh = { cubeVertices, cubeIndices };
	const auto base = static_cast<uint32_t>(mesh.vertices.size());
	const float e = floorHalfExtent;
	// same winding as the cube's top face so the floor faces +y
	mesh.vertices.insert(mesh.vertices.end(), {
		{{ e, floorHeight, -e}},
		{{-e, floorHeight, -e}},
		{{-e, floorHeight,  e}},
		{{ e, floorHeight,  e}},
	});
	mesh.indices.insert(mesh.indices.end(), {
		base + 0, base + 1, base + 2,
		base + 2, base + 3, base + 0
	});

	appendMesh(mesh, buildTreeMesh({.seed = 7}), glm::scale(glm::translate(glm::mat4(1.f), {3.5f, floorHeight, -1.f}), glm::vec3(0.7f)));
	appendMesh(mesh, buildTreeMesh({.seed = 21}), glm::scale(glm::translate(glm::mat4(1.f), {-3.5f, floorHeight, -2.5f}), glm::vec3(0.8f)));
	return mesh;
}

glm::mat4 calculateViewMatrix(const Camera &camera) {
	return glm::lookAt(camera.position, camera.position + camera.forwardVector, camera.upVector);
}
glm::mat4 calculateProjectionMatrix(const Camera& camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}
int main() {
	static constexpr VkFormat kOffscreenFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	static const std::vector<std::string> slang_searchpaths = {
		MYTH_SAMPLES_DIR,
		(kDataDir / "shaders/").string()
	};
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
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name",
			.app_version = {0, 0, 1},
			.engine_version = {0, 0, 1},
			.enableValidation = true,
			.deviceExtensionNames = vulkan_extensions,
			.additionalDeviceFeatures = { .shaderInt64 = VK_TRUE },
			.deviceExtensionFeatureChain = &rayQueryFeatures
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
			}
		})
		.set_slang_cfg({
			.searchpaths = slang_searchpaths
		})
		.with_default_swapchain({
			.width = initialWindowSize.width,
			.height = initialWindowSize.height,
			.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
		})
		.build();

		mythril::Shader raytraceShader = ctx->createShader({
			.source = kDataDir / "raytrace.slang",
			.debugName = "Raytrace Shader"
		});

		mythril::Buffer frameBuffer = ctx->createBuffer({
			.size = sizeof(mythril::GPU::PerFrame),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Frame Uniforms"
		});

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

		const Mesh scene = buildSceneMesh(10.f);

		mythril::Buffer sceneVertexBuffer = ctx->createBuffer({
			.size = sizeof(Vertex) * scene.vertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage | mythril::BufferUsageBits_AccelerationStructureInputReadOnly,
			.storage = mythril::StorageType::Device,
			.initialData = scene.vertices.data(),
			.debugName = "Scene Vertex Buffer"
		});
		mythril::Buffer sceneIndexBuffer = ctx->createBuffer({
			.size = sizeof(uint32_t) * scene.indices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage | mythril::BufferUsageBits_Index | mythril::BufferUsageBits_AccelerationStructureInputReadOnly,
			.storage = mythril::StorageType::Device,
			.initialData = scene.indices.data(),
			.debugName = "Scene Index Buffer"
		});


		// this structure needs to accomadate for the other types besides triangles, ie instance aabb
		mythril::AccelerationStructureGeometry sceneGeometryForAccel = {
			.vertexBuffer = sceneVertexBuffer.handle(),
			.vertexStride = sizeof(Vertex),
			.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
			.numVertices =  static_cast<uint32_t>(scene.vertices.size()) - 1,

			.indexBuffer = sceneIndexBuffer.handle(),
			.indexType = VK_INDEX_TYPE_UINT32,
		};
		mythril::BottomLevelBuildDesc blasBuildSpec = {
			.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
			.geometry = sceneGeometryForAccel,
			.geometryFlags = VK_GEOMETRY_OPAQUE_BIT_KHR,
			.maxPrimitiveCount = static_cast<uint32_t>(scene.indices.size() / 3)
		};
		VkAccelerationStructureBuildSizesInfoKHR blasSizes;
		VkAccelerationStructureGeometryKHR unnused;
		ctx->getBLASBuildInfo(blasBuildSpec, blasSizes, unnused);

		const uint32_t maxStorageBufferSize = ctx->getPhysicalDeviceProperties10().limits.maxStorageBufferRange;
		const uint32_t requiredBLASCount = [&blasSizes, maxStorageBufferSize] {
			const uint32_t count1 = blasSizes.buildScratchSize / maxStorageBufferSize;
			const uint32_t count2 = blasSizes.accelerationStructureSize / maxStorageBufferSize;
			return 1 + (count1 > count2 ? count1 : count2);
		}();
		SAMPLE_ASSERT(requiredBLASCount > 0);
		const auto totalPrimitiveCount = static_cast<uint32_t>(scene.indices.size() / 3);  // divide by 3 since every 3 makes a triangle

		// each of our blases have the same spec but read from a different position in the buffers
		mythril::AccelerationStructureSpec baseSpec = {
			.type = mythril::AccelerationStructureSpec::Type::BLAS,
			.geometryType = mythril::AccelerationStructureSpec::GeometryType::Triangle,
			.geometryFlags = 0,
			.vertexBuffer = sceneVertexBuffer.handle(),
			.numVertices = static_cast<uint32_t>(scene.vertices.size()),
			.vertexStride = sizeof(Vertex),
			.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,

			.indexBuffer = sceneIndexBuffer.handle(),
			.indexType = VK_INDEX_TYPE_UINT32,

			.buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
		};

		const uint32_t desiredPrimitivesPerBLAS = totalPrimitiveCount / requiredBLASCount;
		std::vector<mythril::AccelerationStructure> blases;
		std::vector<VkAccelerationStructureInstanceKHR> instances;
		instances.reserve(requiredBLASCount);
		blases.reserve(requiredBLASCount);
		for (int i = 0; i < requiredBLASCount; i++) {
			const uint32_t firstPrimitive = i * desiredPrimitivesPerBLAS;
			const uint32_t remainingPrimitives = totalPrimitiveCount - firstPrimitive;
			mythril::AccelerationStructureSpec spec = baseSpec;
			spec.buildRange.primitiveOffset = static_cast<uint32_t>(firstPrimitive * 3 * sizeof(uint32_t));
			// switch to remaining once we get to the end of what we can evenly divide
			spec.buildRange.primitiveCount = (desiredPrimitivesPerBLAS < remainingPrimitives) ? desiredPrimitivesPerBLAS : remainingPrimitives;;
			// give each a numbered name
			char debugName[128];
			snprintf(debugName, sizeof(debugName), "BLAS Structure %d", i);
			spec.debugName = debugName;
			blases.push_back(ctx->createAccelerationStructure(spec));
			instances.push_back({
				.transform = {{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}}},
				.instanceCustomIndex = firstPrimitive,
				.mask = 0xFF,
				.instanceShaderBindingTableRecordOffset = 0,
				.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
				.accelerationStructureReference = ctx->gpuAddress(blases.back())
			});
		}

		mythril::Buffer instanceBuf = ctx->createBuffer({
			.size = sizeof(VkAccelerationStructureInstanceKHR) * instances.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_AccelerationStructureInputReadOnly,
			.storage = mythril::StorageType::HostVisible,
			.initialData = instances.data(),
			.debugName = "instances data"
		});
		mythril::AccelerationStructure tlas = ctx->createAccelerationStructure({
			.type = mythril::AccelerationStructureSpec::Type::TLAS,
			.geometryType = mythril::AccelerationStructureSpec::GeometryType::Instances,
			.instancesBuffer = instanceBuf.handle(),
			.buildRange = { .primitiveCount = static_cast<uint32_t>(blases.size()) },
			.buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
			.debugName = "TLAS Accel"
		});

		const auto initalSize = GetSDLWindowFramebufferSize(sdlWindow);
		Camera camera = {
			.position = { 0.f, 0.f, 5.f },
			.forwardVector = { 0.f, 0.f, -1.f },
			.upVector = { 0.f, 1.f, 0.f },
			.aspectRatio = static_cast<float>(initalSize.width)/static_cast<float>(initalSize.height),
			.fov = 80.f,
			.nearPlane = 0.1f,
			.farPlane = 100.f
		};

		// must match the kDebug* constants in raytrace.slang
		static constexpr const char* kDebugModeNames[] = {
			"Lit", "Normals", "Hit Distance", "Primitive ID", "Barycentrics", "Shadow Rays", "Ray Direction"
		};
		static constexpr uint32_t kNumDebugModes = std::size(kDebugModeNames);
		uint32_t debugMode = 0;
		const auto applyDebugMode = [&](uint32_t mode) {
			debugMode = mode;
			char title[128];
			snprintf(title, sizeof(title), "08_RayTracing - [%u] %s (keys 0-%u)", mode, kDebugModeNames[mode], kNumDebugModes - 1);
			SDL_SetWindowTitle(sdlWindow, title);
		};
		applyDebugMode(0);

		mythril::GPU::PerFrame frameData{};

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgTracedShadowOutput = graph.createTexture({
			.size = mythril::RelativeSize{},
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.debugName = "Raytraced output"
		});
		const mythril::GraphBuffer rgFrameData = graph.importBuffer(frameBuffer);
		const mythril::GraphBuffer rgSceneVertices = graph.importBuffer(sceneVertexBuffer);
		const mythril::GraphBuffer rgSceneIndices = graph.importBuffer(sceneIndexBuffer);

		struct TracePass {
			mythril::RayTracingPipelineHandle pipeline;
			mythril::GraphTexture output;
			VkDeviceAddress frame = 0;
			VkDeviceAddress vertices = 0;
			VkDeviceAddress indices = 0;
			uint64_t tlas = 0;
			uint32_t debugMode = 0;
		};
		const auto buildGraph = [&] {
			mythril::GraphBuffer frame = rgFrameData;
			graph.addTransferPass("per_frame_uploads", [&](mythril::TransferBuilder& b) {
				frame = b.update(frame, frameData);
			});

			mythril::GraphTexture traced = rgTracedShadowOutput;
			graph.addRayTracingPass<TracePass>("trace shadows", [&](mythril::RayTracingPassBuilder& b, TracePass& d) {
				traced = b.dependency(traced, mythril::TextureAccess::StorageWrite);
				b.dependency(frame, mythril::BufferAccess::ShaderRead);
				b.dependency(rgSceneVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgSceneIndices, mythril::BufferAccess::ShaderRead);
				d.pipeline = mainPipeline.handle();
				d.output = traced;
				d.frame = ctx->gpuAddress(frameBuffer);
				d.vertices = ctx->gpuAddress(sceneVertexBuffer);
				d.indices = ctx->gpuAddress(sceneIndexBuffer);
				d.tlas = ctx->gpuAddress(tlas);
				d.debugMode = debugMode;
			}, [](const TracePass& d, mythril::RayTracingCmd& cmd) {
				cmd.cmdBindRayTracingPipeline(d.pipeline);
				const mythril::ResolvedTexture tracedOutput = cmd.texture(d.output);
				const mythril::GPU::ShadowPushConstant push = {
					.frame = d.frame,
					// points toward the light, closestHitMain dots it with the surface normal
					.lightDir = glm::vec4(1.5, 2.0, 1.0, 0),
					.vertices = d.vertices,
					.indices = d.indices,
					.outTexture = tracedOutput.index(),
					.tlas = d.tlas,
					.enableShadows = static_cast<uint32_t>(true),
					.debugMode = d.debugMode
				};
				cmd.cmdPushConstants(push);
				// must match the output texture, out of bounds image stores are not discarded without robustness
				cmd.cmdTraceRays({tracedOutput.dimensions.width, tracedOutput.dimensions.height, 1});
			});
			graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
				b.blit(traced, graph.backBuffer());
			});
		};

		bool focused = false;
		SDL_SetWindowRelativeMouseMode(sdlWindow, focused);
		constexpr float kMouseSensitivity = 0.3f;
		constexpr float kBaseCameraSpeed = 4.f;
		constexpr float kShiftCameraSpeed = kBaseCameraSpeed * 3.f;
		// -90 yaw faces -z, matching the initial forward vector
		float yaw = -90.f, pitch = 0.f;

		auto lastTime = std::chrono::steady_clock::now();
		bool quit = false;
		while (!quit) {
			const auto now = std::chrono::steady_clock::now();
			const float deltaTime = std::chrono::duration<float>(now - lastTime).count();
			lastTime = now;

			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_EVENT_QUIT) quit = true;
				if (e.type == SDL_EVENT_KEY_DOWN) {
					if (e.key.key == SDLK_Q) quit = true;
					if (e.key.key == SDLK_C) SDL_SetWindowRelativeMouseMode(sdlWindow, focused = !focused);
					if (e.key.key >= SDLK_0 && e.key.key < SDLK_0 + kNumDebugModes) applyDebugMode(e.key.key - SDLK_0);
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION && focused) {
					yaw += e.motion.xrel * kMouseSensitivity;
					pitch -= e.motion.yrel * kMouseSensitivity;
					pitch = glm::clamp(pitch, -89.0f, 89.0f);
				}
			}
			glm::vec3 front;
			front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
			front.y = sin(glm::radians(pitch));
			front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
			front = glm::normalize(front);
			const glm::vec3 up = {0, 1, 0};
			const glm::vec3 right = glm::normalize(glm::cross(front, up));

			const bool* state = SDL_GetKeyboardState(nullptr);
			const float cameraSpeed = (state[SDL_SCANCODE_LSHIFT] ? kShiftCameraSpeed : kBaseCameraSpeed) * deltaTime;
			if (state[SDL_SCANCODE_W]) camera.position += front * cameraSpeed;
			if (state[SDL_SCANCODE_S]) camera.position -= front * cameraSpeed;
			if (state[SDL_SCANCODE_A]) camera.position -= right * cameraSpeed;
			if (state[SDL_SCANCODE_D]) camera.position += right * cameraSpeed;
			if (state[SDL_SCANCODE_SPACE]) camera.position += up * cameraSpeed;
			if (state[SDL_SCANCODE_LCTRL]) camera.position -= up * cameraSpeed;
			camera.forwardVector = front;
			camera.upVector = up;

			frameData = mythril::GPU::PerFrame {
				.viewInverse = glm::inverse(calculateViewMatrix(camera)),
				.projInverse = glm::inverse(calculateProjectionMatrix(camera)),
			};

			graph.reset();
			buildGraph();
			graph.compile(*ctx);
			graph.execute(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}