//
// Created by Hayden Rivas on 10/4/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/Objects.h"
#include "mythril/RenderGraphBuilder.h"

#include "SDL3/SDL.h"

int main() {
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

	mythril::RenderGraph graph;
	graph.addGraphicsPass("main")
	.attachment({
		.texDesc = ctx->getBackBufferTexture(),
		.clearValue = {1, 0, 0, 1},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		// do absolutely nothing, just begin and end a pass
		cmd.cmdBeginRendering();
		cmd.cmdEndRendering();
	});

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