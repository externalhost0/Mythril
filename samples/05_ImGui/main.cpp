//
// Created by Hayden Rivas on 10/18/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"


#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_sdl3.h"

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
		.width = 1280,
		.height = 720,
		.resizeable = true
	})
	.with_default_swapchain()
	.with_ImGui()
	.build();

	VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
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
	.write({
		.texture = colorTarget.handle(),
		.clearValue = {1, 0, 0, 1},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdDrawImGui();
		cmd.cmdEndRendering();
		cmd.cmdBlitImage(colorTarget.handle(), ctx->getCurrentSwapchainTexture());
		cmd.cmdTransitionLayout(ctx->getCurrentSwapchainTexture(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	});
	graph.compile(*ctx);


	bool quit = false;
	while(!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT) quit = true;
		}

		mythril::Window& window = ctx->getWindow();
		// mandatory for resizeability
		if (ctx->isSwapchainDirty()) {
			ctx->recreateSwapchain();

			extent2D = window.getFramebufferSize();
			colorTarget.resize({extent2D.width, extent2D.height});
			graph.compile(*ctx);
		}


		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ImGui::ShowDemoWindow();
		ImGui::Begin("window");
		{
			if (ImGui::Button("windowed")) {
				window.setWindowMode(mythril::WindowMode::Windowed);
			}
			if (ImGui::Button("fullscreen")) {
				window.setWindowMode(mythril::WindowMode::Fullscreen);
			}
		}
		ImGui::End();


		ImGui::Begin("Debug");
		VkExtent2D framebufferSize = window.getFramebufferSize();
		VkExtent2D windowsize = window.getWindowSize();
		ImGui::Text("Framebuffer: %u, %u", framebufferSize.width, framebufferSize.height);
		ImGui::Text("Windowsize: %u, %u", windowsize.width, windowsize.height);

		const mythril::Dimensions colorTargetDims = colorTarget->getDimensions();
		ImGui::Text("Color Texture Size: %.1u x %.1u", colorTargetDims.width, colorTargetDims.height);

		ImGuiIO& io = ImGui::GetIO();
		ImGui::Text("[ImGui] Display Size: %.1f x %.1f", io.DisplaySize.x, io.DisplaySize.y);
		ImGui::Text("[ImGui] Display Framebuffer Scale: %.1f x %.1f", io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
		ImGui::Text("ImGui Framerate: %.2f", io.Framerate);
		ImGui::End();


		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}