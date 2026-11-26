//
// Created by Hayden Rivas on 10/18/25.
//
#include <SDL3/SDL_vulkan.h>

#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "mythril/RenderGraphBuilder.h"


#include "imgui.h"
#include "../SDL3Usage.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_sdl3.h"

#include "SDL3/SDL.h"

int main() {
	SDL_Window* sdlWindow = BuildSDLWindow(true);
	int w, h;
	SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
	{
		Uint32 sdlExtensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
		auto ctx = mythril::CTXBuilder{}
		               .set_vulkan_cfg({.app_name = "Cool App Name",
						   .engine_name = "Cool Engine Name"})
		               .set_window_surface({.instanceExtensions = std::span(sdlExtensions, sdlExtensionCount),
						   .createFunc = [sdlWindow](VkInstance instance) {
				VkSurfaceKHR surface;
				SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
				return surface; },
						   .destroyFunc = [](VkInstance instance, VkSurfaceKHR surface) { SDL_Vulkan_DestroySurface(instance, surface, nullptr); }})
		               .with_default_swapchain({.width = static_cast<uint32_t>(w),
						   .height = static_cast<uint32_t>(h)})
		               .with_ImGui({.windowInitFunction = [sdlWindow] { ImGui_ImplSDL3_InitForVulkan(sdlWindow); },
						   .windowDestroyFunction = [] { ImGui_ImplSDL3_Shutdown(); }})
		               .build();

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Color Texture"});
		const auto buildGraph = [&] {
			mythril::GraphTexture color = rgColorTarget;
			graph.addGraphicsPass("main", [&](mythril::GraphicsPassBuilder& b, mythril::NoPassData&) { color = b.attachment({.texDesc = color,
																																.clearValue = mythril::ClearValue::color(1.f, 0.f, 0.f, 1.f),
																																.loadOp = mythril::LoadOp::CLEAR,
																																.storeOp = mythril::StoreOp::STORE})
				                                                                                                   .texture; }, [](const mythril::NoPassData&, mythril::GraphicsCmd& cmd) { cmd.cmdDrawImGui(); });
			graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
				b.blit(color, graph.backBuffer());
			});
		};

		bool quit = false;
		while (!quit) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				ImGui_ImplSDL3_ProcessEvent(&e);
				if (e.type == SDL_EVENT_QUIT) quit = true;
			}

			// mandatory for resizeability
			SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({.width = static_cast<uint32_t>(w),
					.height = static_cast<uint32_t>(h)});
			}


			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();
			ImGui::ShowDemoWindow();
			ImGui::Begin("window");
			{
				if (ImGui::Button("windowed")) {
					SDL_SetWindowFullscreen(sdlWindow, false);
				}
				if (ImGui::Button("fullscreen")) {
					SDL_SetWindowFullscreen(sdlWindow, true);
				}
			}
			ImGui::End();


			ImGui::Begin("Debug");
			ImGui::Text("Framebuffer: %u, %u", w, h);
			SDL_GetWindowSize(sdlWindow, &w, &h);
			ImGui::Text("Windowsize: %u, %u", w, h);

			const ImGuiIO& io = ImGui::GetIO();
			ImGui::Text("[ImGui] Display Size: %.1f x %.1f", io.DisplaySize.x, io.DisplaySize.y);
			ImGui::Text("[ImGui] Display Framebuffer Scale: %.1f x %.1f", io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
			ImGui::Text("ImGui Framerate: %.2f", io.Framerate);
			ImGui::End();


			graph.reset();
			buildGraph();
			graph.compile(*ctx);
			graph.execute(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
