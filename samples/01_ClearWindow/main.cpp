//
// Created by Hayden Rivas on 10/4/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/Objects.h"
#include "mythril/RenderGraphBuilder.h"

#include "SDL3/SDL.h"

int main() {
	std::filesystem::path dataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");

	auto ctx = mythril::CTXBuilder{}
	.set_vulkan_cfg({
		.app_name = "Cool App Name",
		.engine_name = "Cool Engine Name"
	})
	.set_window_spec({
		.title = "Cool Window Name",
		.mode = mythril::WindowMode::Windowed,
		.width = 640,
		.height = 480,
		.resizeable = false,
	})
	.with_default_swapchain()
	.build();

	const VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	const mythril::Dimensions dims = {extent2D.width, extent2D.height, 1};
	mythril::Texture colorTarget = ctx->createTexture({
		.dimension = dims,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Device,
		.debugName = "Color Texture"
	});

	mythril::RenderGraph graph;
	graph.addGraphicsPass("main")
	.attachment({
		.texDesc = colorTarget,
		.clearValue = {1, 0, 0, 1},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		// do absolutely nothing, just begin and end a pass
		cmd.cmdBeginRendering();
		cmd.cmdEndRendering();
	});
	graph.addIntermediate("present")
	.blit(colorTarget, ctx->getBackBufferTexture())
	.finish();

	graph.compile(*ctx);

	bool quit = false;
	while(!quit) {
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