//
// Created by Hayden Rivas on 10/18/25.
//
#include <mythril/CTXBuilder.h>
#include <mythril/RenderGraphBuilder.h>


#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_glfw.h>

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
		.with_ImGui()
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
			cmd.cmdDrawImGui();
		});
	graph.compile(*ctx);


	bool quit = false;
	while(!quit) {
		if (ctx->pollAndCheck()) quit = true;

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGui::ShowDemoWindow();

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}