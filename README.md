# Mythril Framework
Mythril is C++23 Vulkan rendering framework for Windows, Linux, and MacOS. 
It aims to provide a easy to setup and use frontfacing API for Vulkan, aswell as some bonus features described below.


### Features:
* Rendergraph implementation.
* [BARELY] Shader reflection.
* [NOT YET DONE] Tracy, profiles the rendering logic. Learn more about Tracy [here](https://github.com/wolfpld/tracy).


### Minimal Example:
```
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
		.width = 640,
		.height = 480,
		.resizeable = false,
	})
	.build();

	VkExtent2D extent2D = {1280, 720};
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
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		// do absolutely nothing, just begin and end a pass
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
```

## Building
* C++ 20
* CMake 3.28+
* Vulkan SDK 1.4.3+

## Installing
idk use cmake
