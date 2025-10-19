//
// Created by Hayden Rivas on 10/4/25.
//
#include <mythril/CTXBuilder.h>
#include <mythril/RenderGraphBuilder.h>

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
		.resizeable = true,
	})
	.build();

	VkExtent2D extent2D = {1280*2, 720*2};
	mythril::InternalTextureHandle colorTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Color Texture"
	});

	mythril::RenderGraph graph;
	graph.addPass("main", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.clearValue = {1, 0, 0, 1},
		.loadOp = mythril::LoadOperation::CLEAR,
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		// do absolutely nothing, just begin and end a pass
	});
	graph.compile(*ctx);


	bool quit = false;
	while(!quit) {
		if (ctx->pollAndCheck()) quit = true;

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}