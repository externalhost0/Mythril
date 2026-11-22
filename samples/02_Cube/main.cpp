//
// Created by Hayden Rivas on 10/12/25.
//

#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "mythril/RenderGraphBuilder.h"

#include <vector>

#include "glm/glm.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "GPUStructs.h"
#include "../SDL3Usage.h"

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

glm::mat4 calculateViewMatrix(const Camera &camera) {
	return glm::lookAt(camera.position, camera.position + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
}
glm::mat4 calculateProjectionMatrix(const Camera& camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}
int main() {
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	static const std::vector<std::string> slang_searchpaths = {
		// for shader_types.h to be included in a shader file
		MYTH_SAMPLES_DIR,
		kDataDir.string()
	};
	SDL_Window* sdlWindow = BuildSDLWindow(false);
	auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
	{
		Uint32 sdlExtensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name"
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
			.height = initialWindowSize.height
		})
		.build();

		mythril::Shader standardShader = ctx->createShader({
			.source = kDataDir / "BasicRed.slang",
			.debugName = "Red Object Shader"
		});
		mythril::GraphicsPipeline mainPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {standardShader},
			.fragmentShader = {standardShader},
			.topology = mythril::TopologyMode::TRIANGLE,
			.polygon = mythril::PolygonMode::FILL,
			.blend = mythril::BlendingMode::OFF,
			.cull = mythril::CullMode::BACK,
			.multisample = mythril::SampleCount::X1,
			.debugName = "Main Pipeline"
		});

		mythril::Buffer cubeVertexBuffer = ctx->createBuffer({
			.size = sizeof(Vertex) * cubeVertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = cubeVertices.data(),
			.debugName = "Cube Vertex Buffer"
		});
		mythril::Buffer cubeIndexBuffer = ctx->createBuffer({
			.size = sizeof(uint32_t) * cubeIndices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
			.storage = mythril::StorageType::Device,
			.initialData = cubeIndices.data(),
			.debugName = "Cube Index Buffer"
		});

		auto startTime = std::chrono::high_resolution_clock::now();

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgColorTarget = graph.createTexture({
			.size = mythril::RelativeSize{},
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.debugName = "Color Texture"
		});
		const mythril::GraphTexture rgDepthTarget = graph.createTexture({
			.size = mythril::RelativeSize{},
			.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
			.debugName = "Depth Texture"
		});
		const mythril::GraphBuffer rgCubeVertices = graph.importBuffer(cubeVertexBuffer);
		const mythril::GraphBuffer rgCubeIndices = graph.importBuffer(cubeIndexBuffer);

		struct MainPass {
			mythril::GraphicsPipelineHandle pipeline;
			mythril::BufferHandle indexBuffer;
			uint32_t indexCount = 0;
			glm::mat4 mvp{};
			VkDeviceAddress vertices = 0;
		};
		const auto buildGraph = [&] {
			mythril::GraphTexture color = rgColorTarget;
			graph.addGraphicsPass<MainPass>("main", [&](mythril::GraphicsPassBuilder& b, MainPass& d) {
				color = b.attachment({
					.texDesc = color,
					.clearValue = mythril::ClearValue::color(0.2f, 0.2f, 0.2f, 1.f),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.attachment({
					.texDesc = rgDepthTarget,
					.clearValue = mythril::ClearValue::depth(1.f, 0),
					.loadOp = mythril::LoadOp::CLEAR,
				});
				b.dependency(rgCubeVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgCubeIndices, mythril::BufferAccess::IndexRead);

				auto frameWindowSize = GetSDLWindowFramebufferSize(sdlWindow);

				Camera camera = {
						.position = { 0.f, 0.f, 5.f },
						.aspectRatio = (float)frameWindowSize.width/(float)frameWindowSize.height,
						.fov = 80.f,
						.nearPlane = 0.1f,
						.farPlane = 100.f
				};

				// rotating cube!
				auto currentTime = std::chrono::high_resolution_clock::now();
				float time = std::chrono::duration<float>(currentTime - startTime).count();
				glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
				model = glm::rotate(model, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
				d.pipeline = mainPipeline.handle();
				d.indexBuffer = cubeIndexBuffer.handle();
				d.indexCount = static_cast<uint32_t>(cubeIndices.size());
				d.mvp = calculateProjectionMatrix(camera) * calculateViewMatrix(camera) * model;
				d.vertices = ctx->gpuAddress(cubeVertexBuffer);
			}, [](const MainPass& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				const mythril::GPU::PushConstant push = {
						.mvp = d.mvp,
						.vertexBufferAddress = d.vertices
				};
				cmd.cmdPushConstants(push);
				cmd.cmdBindIndexBuffer(d.indexBuffer);
				cmd.cmdDrawIndexed(d.indexCount);
			});
			graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
				b.blit(color, graph.backBuffer());
			});
		};

		bool quit = false;
		while(!quit) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_EVENT_QUIT) quit = true;
			}

			graph.reset();
			buildGraph();
			graph.compile(*ctx);
			graph.execute(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
