//
// Created by Hayden Rivas on 7/1/26.
//

#include <mythril/CTX.h>
#include <mythril/CTXBuilder.h>

#include <SDL3/SDL_vulkan.h>
#include <glm/vec3.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "../SDL3Usage.h"
#include "glm/detail/type_mat4x4.hpp"

#include "GPUStructs.h"

struct Vertex {
	glm::vec3 position;
};
struct Camera {
	glm::vec3 position;
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

struct Mesh {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
};
Mesh buildSceneMesh(float floorHalfExtent, float floorHeight = -1.f) {
	Mesh mesh = { cubeVertices, cubeIndices };
	const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
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
	return mesh;
}

glm::mat4 calculateViewMatrix(const Camera &camera) {
	return glm::lookAt(camera.position, camera.position + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
}
glm::mat4 calculateProjectionMatrix(const Camera& camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}
int main() {
	static constexpr VkFormat kOffscreenFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	static const std::vector<std::string> slang_searchpaths = {
		"../../include/",
		"../include/",
		(kDataDir / "shaders/").string()
	};
	SDL_Window* sdlWindow = BuildSDLWindow(true);
	const auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
	static const std::vector<const char*> vulkan_extensions = {
		VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
		VK_KHR_RAY_QUERY_EXTENSION_NAME
	};
	{
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name",
			.app_version = {0, 0, 1},
			.engine_version = {0, 0, 1},
			.enableValidation = true,
			.deviceExtensions = vulkan_extensions,
		})
		.set_window_surface([sdlWindow](VkInstance instance) {
			VkSurfaceKHR surface;
			SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
			return surface;
		},
		[](VkInstance instance, VkSurfaceKHR surface_khr) {
			SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
		})
		.set_slang_cfg({
			.searchpaths = slang_searchpaths
		})
		.with_default_swapchain({
			.width = initialWindowSize.width,
			.height = initialWindowSize.height,
			.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR,
		})
		.build();

		mythril::Shader standardShader = ctx->createShader({
			.source = kDataDir / "main.slang",
			.debugName = "Main Shader"
		});
		mythril::Shader raytraceShader = ctx->createShader({
			.source = kDataDir / "raytrace.slang",
			.debugName = "Raytrace Shader"
		});


		mythril::RaytracingPipeline mainPipeline = ctx->createRaytracingPipeline({
			.rayGenShader = { raytraceShader, "raygenMain" },
			.missShader = { raytraceShader, "missMain" },
			.hitShader = { raytraceShader, "hitMain" } ,
			.debugName = "Main Pipeline"
		});

		const Mesh scene = buildSceneMesh(10.f);

		mythril::Buffer sceneVertexBuffer = ctx->createBuffer({
			.size = sizeof(Vertex) * scene.vertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = scene.vertices.data(),
			.debugName = "Scene Vertex Buffer"
		});
		mythril::Buffer sceneIndexBuffer = ctx->createBuffer({
			.size = sizeof(uint32_t) * scene.indices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
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
		mythril::BottomLevelBuildSpec blasBuildSpec = {
			.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
			.geometry = sceneGeometryForAccel,
			.geometryFlags = VK_GEOMETRY_OPAQUE_BIT_KHR
		};
		VkAccelerationStructureBuildSizesInfoKHR blasSizes = ctx->getBLASBuildInfo(blasBuildSpec);

		const uint32_t maxStorageBufferSize = ctx->getPhysicalDeviceProperties10().limits.maxStorageBufferRange;
		const uint32_t requiredBLASCount = [&blasSizes, maxStorageBufferSize] {
			const uint32_t count1 = blasSizes.buildScratchSize / maxStorageBufferSize;
			const uint32_t count2 = blasSizes.accelerationStructureSize / maxStorageBufferSize;
			return 1 + (count1 > count2 ? count1 : count2);
		}();
		ASSERT(requiredBLASCount > 0);
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

		struct Instance {
			glm::mat3x4 transform;
			uint32_t customIndex = 0;
			mythril::AccelerationStructureHandle blas;
		};

		const uint32_t desiredPrimitivesPerBLAS = totalPrimitiveCount / requiredBLASCount;
		std::vector<mythril::AccelerationStructure> blases;
		std::vector<Instance> instances;
		instances.reserve(requiredBLASCount);
		blases.reserve(requiredBLASCount);
		for (int i = 0; i < requiredBLASCount; i++) {
			const uint32_t remainingPrimitives = totalPrimitiveCount - i;
			mythril::AccelerationStructureSpec spec = baseSpec;
			spec.buildRange.primitiveOffset = static_cast<uint32_t>(i * 3 * sizeof(uint32_t));
			// switch to remaining once we get to the end of what we can evenly divide
			spec.buildRange.primitiveCount = (desiredPrimitivesPerBLAS < remainingPrimitives) ? desiredPrimitivesPerBLAS : remainingPrimitives;;
			// give each a numbered name
			char debugName[128];
			snprintf(debugName, sizeof(debugName), "BLAS Structure %d", i);
			spec.debugName = debugName;
			blases[i] = ctx->createAccelerationStructure(spec);
			instances[i] = {
				.transform = glm::scale(glm::mat4(1), glm::vec3(1.f)),
				.customIndex = 0,
				.blas = blases[i].handle()
			};
		}


		mythril::Buffer instanceBuf = ctx->createBuffer({
			.size = sizeof(Instance) * instances.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_AccelerationStructureInput,
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


		const auto startTime = std::chrono::high_resolution_clock::now();
		mythril::RenderGraph graph;
		graph.addGraphicsPass("final")
		.attachment({
			.texDesc = ctx->getBackBufferTexture(),
			.clearValue = mythril::ClearValue::color(0.2f, 0.2f, 0.2f, 1.f),
			.loadOp = mythril::LoadOp::CLEAR,
			.storeOp = mythril::StoreOp::STORE
		})
		.execute([&](mythril::CommandBuffer& cmd) {
			cmd.cmdBindRaytracingPipeline(mainPipeline);

			auto frameWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
			Camera camera = {
					.position = { 0.f, 0.f, 5.f },
					.aspectRatio = (float)frameWindowSize.width/(float)frameWindowSize.height,
					.fov = 80.f,
					.nearPlane = 0.1f,
					.farPlane = 100.f
			};
			auto currentTime = std::chrono::high_resolution_clock::now();
			float time = std::chrono::duration<float>(currentTime - startTime).count();
			glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
			model = glm::rotate(model, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
			GPU::PushConstant push = {
					.mvp = calculateProjectionMatrix(camera) * calculateViewMatrix(camera) * model,
					.vertexBufferAddress = ctx->gpuAddress(sceneVertexBuffer.handle())
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(sceneIndexBuffer);
			cmd.cmdDrawIndexed(scene.indices.size());
		});
		graph.compile(*ctx);

		bool quit = false;
		while (!quit) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_EVENT_QUIT) quit = true;
			}
			graph.executeEXT(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}