//
// Created by Hayden Rivas on 10/18/25.
//
#include <SDL3/SDL_vulkan.h>

#include "mythril/CTXBuilder.h"
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
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name"
		})
		.set_window_surface([sdlWindow](VkInstance instance) {
			VkSurfaceKHR surface;
			SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
			return surface;
		},
		[](VkInstance instance, VkSurfaceKHR surface_khr) {
			SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
		})
		.with_default_swapchain({
			.width = static_cast<uint32_t>(w),
			.height = static_cast<uint32_t>(h)
		})
		.with_ImGui({
			.windowInitFunction = [sdlWindow] { ImGui_ImplSDL3_InitForVulkan(sdlWindow); },
			.windowDestroyFunction = [] { ImGui_ImplSDL3_Shutdown(); }
		})
		.build();

		const mythril::Dimensions dims = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
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
			cmd.cmdBeginRendering();
			cmd.cmdDrawImGui();
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
				ImGui_ImplSDL3_ProcessEvent(&e);
				if (e.type == SDL_EVENT_QUIT) quit = true;
			}

			// mandatory for resizeability
			SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({
					.width = static_cast<uint32_t>(w),
					.height = static_cast<uint32_t>(h)
				});

				colorTarget.resize({static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
				graph.compile(*ctx);
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

			const mythril::Dimensions colorTargetDims = colorTarget->getDimensions();
			ImGui::Text("Color Texture Size: %.1u x %.1u", colorTargetDims.width, colorTargetDims.height);

			const ImGuiIO& io = ImGui::GetIO();
			ImGui::Text("[ImGui] Display Size: %.1f x %.1f", io.DisplaySize.x, io.DisplaySize.y);
			ImGui::Text("[ImGui] Display Framebuffer Scale: %.1f x %.1f", io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
			ImGui::Text("ImGui Framerate: %.2f", io.Framerate);
			ImGui::End();


			mythril::CommandBuffer& cmd = ctx->acquireCommand(mythril::CommandBuffer::Type::Graphics);
			graph.execute(cmd);
			ctx->submitCommand(cmd);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}