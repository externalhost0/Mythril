# Mythril Framework
Mythril is an opinionated yet generic Vulkan rendering framework for Windows, Linux, and MacOS. 
It aims to provide a easy to create context and highly abstracted API for Vulkan, aswell as some bonus features described below.

#### If you want to get started using Mythril in your project, take a read through the [getting_started.md](/docs/getting_started.md)

### Features:
* RenderGraph implementation, automatically builds and optimizes your described frame.
* Fully Bindless design for textures & samplers.
* Automatic shader reflection.
* RAII Object System, all Vulkan objects are cleaned up automatically.
* Supported plugin system for commonly used dev tools, currently includes ImGui and Tracy.
* BYOW (Bring Your Own Window), you can use whatever windowing library you like easily. (SDL3/GLFW/any native api)

![Sample 07 Screenshot][sample_07_img]

### Minimal Example:
```cpp
int main() {
	SDL_Window* sdlWindow = BuildSDLWindow(false);
	const auto [width, height] = GetSDLWindowFramebufferSize(sdlWindow);
	{
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name"
		})
		.set_window_surface(
		[sdlWindow](VkInstance instance) {
			VkSurfaceKHR surface;
			SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
			return surface;
		},
		[](VkInstance instance, VkSurfaceKHR surface_khr) {
			SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
		})
		.with_default_swapchain({
			.width = windowSize.width,
			.height = windowSize.height,
			.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR
		})
		.build();

		mythril::RenderGraph graph;
		graph.addGraphicsPass("main")
		.attachment({
			.texDesc = ctx->getBackBufferTexture(),
			.clearValue = {1, 0, 0, 1},
			.loadOp = mythril::LoadOp::CLEAR,
			.storeOp = mythril::StoreOp::STORE
		})
		.execute([&](mythril::CommandBuffer& cmd) {
			// do absolutely nothing, just begin and end a pass
		});

		graph.compile(*ctx);

		bool quit = false;
		while(!quit) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_EVENT_QUIT) quit = true;
			}

			mythril::CommandBuffer& cmd = ctx->acquireCommand(mythril::CommandBuffer::Type::Graphics);
			graph.execute(cmd);
			ctx->submitCommand(cmd);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
```
![Minimal Example Screenshot][basic_window_img]

## Requirements
* C++20 compiler (tested with clang 16.0.0, gcc 13.3.0, MSVC v143)
* CMake 3.28+
* Vulkan SDK 1.4.335.0+ (must include Slang)

The Vulkan SDK is the only external prerequisite. Slang is consumed from the
SDK at both build and runtime, so consumers do not need to ship any extra
shared libraries themselves.

## Using Mythril in your project

### Add as a subdirectory (recommended)
Clone Mythril (e.g. as a submodule under `third_party/mythril`) and consume it
from your own `CMakeLists.txt`:
```cmake
add_subdirectory(third_party/mythril)
target_link_libraries(my_app PRIVATE mythril::mythril)
```
Publicly exposed headers are `Vulkan`, `volk`, `fmt`, `VMA`, and
(when enabled) `Tracy`, these will be visible to consumers at compile time. 

### Building the samples
Samples are off by default. To build and run them locally:
```bash
cmake --preset debug
cmake --build build/debug
```
The `debug` / `release` presets enable `MYTH_BUILD_SAMPLES`,
`MYTH_ENABLE_IMGUI_STANDARD`, and `MYTH_ENABLE_TRACY`.

### CMake Options
| Option                       | Default | Description                                                            |
|------------------------------|---------|------------------------------------------------------------------------|
| `MYTH_BUILD_SAMPLES`         | `OFF`   | Build the sample applications under `samples/`.                        |
| `MYTH_INSTALL`               | `OFF`   | Generate `install()` rules and `mythrilConfig.cmake` (experimental).   |
| `MYTH_ENABLE_IMGUI_STANDARD` | `OFF`   | Fetch ImGui (main branch) and enable mythril's ImGuiPlugin.            |
| `MYTH_ENABLE_IMGUI_DOCKING`  | `OFF`   | Fetch ImGui (docking branch) and enable mythril's ImGuiPlugin.         |
| `MYTH_ENABLE_TRACY`          | `OFF`   | Fetch Tracy and enable mythril's TracyPlugin.                          |
| `MYTH_ENABLE_TRACY_GPU`      | `OFF`   | Requires `MYTH_ENABLE_TRACY=ON`. GPU timing (experimental).            |
| `MYTH_USE_LOCAL_SLANG`       | `OFF`   | Use a locally-built Slang (`MYTH_LOCAL_SLANG_DIR`) instead of the SDK. |

> Note: `MYTH_ENABLE_IMGUI_STANDARD` and `MYTH_ENABLE_IMGUI_DOCKING` are mutually exclusive.
> Samples that require an optional feature (e.g. `05_ImGui` needs
> `MYTH_ENABLE_IMGUI_STANDARD`) are skipped when the requirement is not satisfied.



## License
This project is distributed under the **Mozilla Public License Version 2.0**, please see `LICENSE.txt` for more.

# Acknowledgments

- [LightweightVK](https://github.com/corporateshark/lightweightvk/tree/master) - Basically why Mythril exists, I loved how simply lightweightvk is but found we could abstract even more out of it.

<!-- image definitions -->
[basic_window_img]: docs/img/basic_window.png
[sample_07_img]: docs/img/sample07.png
