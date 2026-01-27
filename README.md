# Mythril Framework
Mythril is C++20 Vulkan rendering framework for Windows, Linux, and MacOS. 
It aims to provide a easy to create and highly abstracted API for Vulkan, aswell as some bonus features described below.

### Features:
* RenderGraph implementation.
* Automatic shader reflection.
* Abstracted and easy to use resource descriptors.


### Minimal Example:
```cpp
#include <mythril/CTXBuilder.h>
#include <mythril/RenderGraphBuilder.h>

#include <SDL3/SDL.h>

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
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
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
```
![Minimal Example Screenshot][basic_window_img]

## Building
* C++ 20 (Clang 16.0.0+)
* CMake 3.28+
* Vulkan SDK 1.4.3+

## Installing
You can easily include with CPM.
```cmake
CPMAddPackage(
    NAME mythril
    GITHUB_REPOSITORY "externalhost0/Mythril"
    GIT_TAG main
)
```
You could also clone as a submodule and add as a subdirectory.
```cmake
add_subdirectory(mythril)
```

### CMake Options
| Option                       | Default | Description                                                                |
|------------------------------|---------|----------------------------------------------------------------------------|
| `MYTH_RUN_SAMPLES`           | `ON`    | Enables sample apps.                                                       |
| `MYTH_ENABLE_IMGUI_STANDARD` | `OFF`   | Installs ImGui (Main Branch) and enables mythril's ImGuiPlugin             |
| `MYTH_ENABLE_IMGUI_DOCKING`  | `OFF`   | Installs ImGui (Docking Branch) and enables mythril's ImGuiPlugin          |
| `MYTH_ENABLE_TESTS`          | `OFF`   | Builds tests directory to be run by CMake.                                 |
| `MYTH_ENABLE_TRACY`          | `OFF`   | Installs Tracy and enabled mythrils' TracyPlugin                           |
| `MYTH_ENABLE_TRACY_GPU`      | `OFF`   | Requires ENABLE_TRACY to be ON, allows GPU timing (CURRENTLY EXPERIMENTAL) |
> Note: `MYTH_ENABLE_IMGUI_STANDARD` and `MYTH_ENABLE_IMGUI_DOCKING` are mutually exclusive!


## License
This project is distributed under the **Mozilla Public License Version 2.0**, please see `LICENSE.txt` for more.

# Acknowledgments

- [LightweightVK](https://github.com/corporateshark/lightweightvk/tree/master) - Basically why Mythril exists, I loved how simply lightweightvk is but found we could abstract even more out of it.

<!-- image definitions -->
[basic_window_img]: docs/img/basic_window.png
