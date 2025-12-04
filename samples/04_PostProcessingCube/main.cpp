//
// Created by Hayden Rivas on 10/17/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"

#include <vector>

#include "glm/glm.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include "SDL3/SDL.h"

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
		{{1.f,  -1.f, -1.f}}, // B 1
		{{1.f,  1.f,  -1.f}}, // C 2
		{{-1.f, 1.f,  -1.f}}, // D 3

		// back face
		{{-1.f, -1.f, 1.f}}, // E 4
		{{1.f,  -1.f, 1.f}}, // F 5
		{{1.f,  1.f,  1.f}}, // G 6
		{{-1.f, 1.f,  1.f}},  // H 7

		// left face
		{{-1.f, 1.f,  -1.f}}, // D 8
		{{-1.f, -1.f, -1.f}}, // A 9
		{{-1.f, -1.f, 1.f}}, // E 10
		{{-1.f, 1.f,  1.f}}, // H 11

		// right face
		{{1.f,  -1.f, -1.f}}, // B 12
		{{1.f,  1.f,  -1.f}}, // C 13
		{{1.f,  1.f,  1.f}}, // G 14
		{{1.f,  -1.f, 1.f}}, // F 15

		// bottom face
		{{-1.f, -1.f, -1.f}}, // A 16
		{{1.f,  -1.f, -1.f}}, // B 17
		{{1.f,  -1.f, 1.f}}, // F 18
		{{-1.f, -1.f, 1.f}}, // E 19

		// top face
		{{1.f,  1.f,  -1.f}}, // C 20
		{{-1.f, 1.f,  -1.f}}, // D 21
		{{-1.f, 1.f,  1.f}}, // H 22
		{{1.f,  1.f,  1.f}}, // G 23
};

const std::vector<uint32_t> cubeIndices = {
		// front and back
		0, 3, 2,
		2, 1, 0,
		4, 5, 6,
		6, 7, 4,
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

glm::mat4 calculateViewMatrix(Camera camera) {
	return glm::lookAt(camera.position, camera.position + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
}

glm::mat4 calculateProjectionMatrix(Camera camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}

int main() {
	auto ctx = mythril::CTXBuilder{}
	.set_info_spec({
		.app_name = "Cool App Name",
		.engine_name = "Cool Engine Name"
	})
	.set_window_spec({
		.title = "Cool Window Name",
		.mode = mythril::WindowMode::Windowed,
		.width = 1280,
		.height = 720,
		.resizeable = false
	})
	.set_shader_search_paths({
		"../../include/"
	})
	.build();

	VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	mythril::InternalTextureHandle colorTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X4,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Memoryless,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Color Texture"
	});
	mythril::InternalTextureHandle resolveColorTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Color Resolve Texture"
	});
	mythril::InternalTextureHandle postColorTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Post Process Texture"
	});
	mythril::InternalTextureHandle depthTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X4,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Memoryless,
		.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.debugName = "Depth Texture"
	});

	mythril::InternalShaderHandle standardShader = ctx->createShader({
		.filePath = "BasicRed.slang",
		.debugName = "Red Object Shader"
	});
	mythril::InternalGraphicsPipelineHandle mainPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.cull = mythril::CullMode::BACK,
		.multisample = mythril::SampleCount::X4,
		.debugName = "Main Pipeline"
	});
	mythril::InternalShaderHandle postProcessingShader = ctx->createShader({
		.filePath = "FullscreenPost.slang",
		.debugName = "Fullscreen Shader"
	});
	mythril::InternalGraphicsPipelineHandle postPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {postProcessingShader},
		.fragmentShader = {postProcessingShader},
		.debugName = "Post Processing Pipeline"
	});

	mythril::InternalBufferHandle cubeVertexBuffer = ctx->createBuffer({
		.size = sizeof(Vertex) * cubeVertices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.initialData = cubeVertices.data(),
		.debugName = "Cube Vertex Buffer"
	});
	mythril::InternalBufferHandle cubeIndexBuffer = ctx->createBuffer({
		.size = sizeof(uint32_t) * cubeIndices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
		.storage = mythril::StorageType::Device,
		.initialData = cubeIndices.data(),
		.debugName = "Cube Index Buffer"
	});


	auto startTime = std::chrono::high_resolution_clock::now();

	mythril::RenderGraph graph;
	graph.addPass("main", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.clearValue = {0.2f, 0.2f, 0.2f, 1.f},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE,
		.resolveTexture = resolveColorTarget
	})
	.write({
		.texture = depthTarget,
		.clearValue = {1.f, 0},
		.loadOp = mythril::LoadOperation::CLEAR
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(mainPipeline);

		VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		Camera camera = {
				.position = {0.f, 0.f, 5.f},
				.aspectRatio = (float) windowSize.width / (float) windowSize.height,
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 100.f
		};

		// rotating cube!
		auto currentTime = std::chrono::high_resolution_clock::now();
		float time = std::chrono::duration<float>(currentTime - startTime).count();
		glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
		model = glm::rotate(model, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
		GPU::GeometryPushConstant push {
			.mvp = calculateProjectionMatrix(camera) * calculateViewMatrix(camera) * model,
			.vertexBufferAddress = ctx->gpuAddress(cubeVertexBuffer)
		};
		cmd.cmdPushConstants(push);
		cmd.cmdBindIndexBuffer(cubeIndexBuffer);
		cmd.cmdDrawIndexed(cubeIndices.size());
	});
	graph.addPass("post_processing", mythril::PassSource::Type::Graphics)
	.write({
		.texture = postColorTarget,
		.loadOp = mythril::LoadOperation::NO_CARE,
		.storeOp = mythril::StoreOperation::STORE
	})
	.read({
		.texture = resolveColorTarget
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(postPipeline);

		auto currentTime = std::chrono::high_resolution_clock::now();
		float time = std::chrono::duration<float>(currentTime - startTime).count();
		GPU::FullscreenPushConstant push {
			.colorTexture = resolveColorTarget.index(),
			.linearSampler = 0,
			.time = time
		};
		cmd.cmdPushConstants(push);
		cmd.cmdDraw(3);
	});
	graph.compile(*ctx);


	bool quit = false;
	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) quit = true;
		}

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}
