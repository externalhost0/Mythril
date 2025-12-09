//
// Created by Hayden Rivas on 10/11/25.
//
#include "CTX.h"
#include "Plugins.h"

#include <array>
#include <cstdio>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_sdl3.h>

namespace mythril {
	void ImGuiPlugin::onInit(CTX& ctx, SDL_Window* sdlWindow, VkFormat format) {
		this->_isEnabeld = true;
		this->_requestedFormat = format;
		_ctx = &ctx;
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#ifdef MYTH_ENABLED_IMGUI_DOCKING
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif

		VkDescriptorPoolSize pool_sizes[] = {
				{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
				{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
				{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
				{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
		};

		VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool_info.maxSets = 1000;
		pool_info.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
		pool_info.pPoolSizes = pool_sizes;
		VK_CHECK(vkCreateDescriptorPool(ctx._vkDevice, &pool_info, nullptr, &this->_descriptorPool));

		ImGui_ImplVulkan_InitInfo vulkanInitInfo = {
				.ApiVersion = VK_API_VERSION_1_3,
				.Instance = ctx._vkInstance,
				.PhysicalDevice = ctx._vkPhysicalDevice,
				.Device = ctx._vkDevice,
				.QueueFamily = ctx._graphicsQueueFamilyIndex,
				.Queue = ctx._vkGraphicsQueue,
				.DescriptorPool = this->_descriptorPool,
				.MinImageCount = 2,
				.ImageCount = 2,
				.PipelineInfoMain = {
						.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
						.PipelineRenderingCreateInfo = {
								.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
								.pNext = nullptr,
								.colorAttachmentCount = 1,
								.pColorAttachmentFormats = &format
						}
				},
				.UseDynamicRendering = true,
				.Allocator = nullptr,
				.CheckVkResultFn = [](VkResult err) {
					if (err == 0) return;
					fprintf(stderr, "[ImGui via Vulkan] Error: VkResult = %d\n", err);
					if (err < 0) abort();
				},
		};
		ImGui_ImplSDL3_InitForVulkan(sdlWindow);
		ImGui_ImplVulkan_Init(&vulkanInitInfo);
		// to pull in ctx to use in aliased functions
		auto* data = new MyUserData {
			.ctx = &ctx,
			.sampler = _ctx->_samplerPool.get(_ctx->_dummyLinearSamplerHandle)->getSampler(),
			.handleMap = {}
		};
		ImGui::GetIO().UserData = data;
	}
	void ImGuiPlugin::onDestroy() {
		auto* data = reinterpret_cast<MyUserData*>(ImGui::GetIO().UserData);
		delete data;
		ImGui_ImplVulkan_Shutdown();
		// destroying the descriptor pool we made must come after ImplVulkan_Shutdown()
		vkDestroyDescriptorPool(_ctx->_vkDevice, _descriptorPool, nullptr);
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
		this->_isEnabeld = false;
	}
}