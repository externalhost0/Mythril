//
// Created by Hayden Rivas on 10/11/25.
//
#include "mythril/CTX.h"
#include "Plugins.h"

#include <array>
#include <cstdio>

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include "mythril/CTXBuilder.h"

namespace mythril {
	namespace {
		class ScopedImGuiContext {
		public:
			explicit ScopedImGuiContext(ImGuiContext* next) :
			    _previous(ImGui::GetCurrentContext()) {
				ASSERT_MSG(next, "Cannot switch to a null ImGui context.");
				if (_previous != next) {
					ImGui::SetCurrentContext(next);
				}
			}

			~ScopedImGuiContext() {
				if (ImGui::GetCurrentContext() != _previous) {
					ImGui::SetCurrentContext(_previous);
				}
			}

		private:
			ImGuiContext* _previous = nullptr;
		};
	} // namespace

	void ImGuiPlugin::onInit(CTX& ctx, ImGuiPluginSpec spec) {
		ASSERT_MSG(_activePlugin == nullptr || _activePlugin == this, "Only one Mythril ImGui plugin can be active at a time.");

		this->_requestedFormat = spec.format;
		this->_funcWindowInit = spec.windowInitFunction;
		this->_funcWindowDestroy = spec.windowDestroyFunction;

		_ctx = &ctx;
		IMGUI_CHECKVERSION();
		_backendContext = ImGui::CreateContext();
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

		VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool_info.maxSets = 1000;
		pool_info.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
		pool_info.pPoolSizes = pool_sizes;
		VK_CHECK(vkCreateDescriptorPool(ctx._vkDevice, &pool_info, nullptr, &this->_descriptorPool));

		// todo: have the image counts be variable
		ImGui_ImplVulkan_InitInfo vulkanInitInfo = {
		    .ApiVersion = VK_API_VERSION_1_3,
		    .Instance = ctx._vkInstance,
		    .PhysicalDevice = ctx._vkPhysicalDevice,
		    .Device = ctx._vkDevice,
		    .QueueFamily = ctx._graphicsQueueFamilyIndex,
		    .Queue = ctx._vkGraphicsQueue,
		    .DescriptorPool = this->_descriptorPool,
		    .MinImageCount = 2,
		    .ImageCount = kNUM_FRAMES_IN_FLIGHT,
		    .PipelineInfoMain =
		            {.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
		             .PipelineRenderingCreateInfo =
		                     {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = nullptr, .colorAttachmentCount = 1, .pColorAttachmentFormats = &this->_requestedFormat}},
		    .UseDynamicRendering = true,
		    .Allocator = nullptr,
		    .CheckVkResultFn = [](VkResult err) {
			    if (err == 0)
				    return;
			    fprintf(stderr, "[ImGui via Vulkan] Error: VkResult = %d\n", err);
			    if (err < 0)
				    abort();
		    },
		};
		this->_funcWindowInit();
		ImGui_ImplVulkan_Init(&vulkanInitInfo);
		_defaultSampler = _ctx->_samplerPool.get(_ctx->_dummyLinearSampler.handle())->getSampler();
		_activePlugin = this;
		this->_isEnabeld = true;
	}
	void ImGuiPlugin::onDestroy() {
		VK_CHECK(vkDeviceWaitIdle(_ctx->_vkDevice));
		_ctx->waitDeferredTasks();
		clearImageDescriptors();
		{
			ScopedImGuiContext context(_backendContext);
			ImGui_ImplVulkan_Shutdown();
		}
		// destroying the descriptor pool we made must come after ImplVulkan_Shutdown()
		vkDestroyDescriptorPool(_ctx->_vkDevice, _descriptorPool, nullptr);
		{
			ScopedImGuiContext context(_backendContext);
			this->_funcWindowDestroy();
		}
		ImGui::DestroyContext(_backendContext);
		if (_activePlugin == this) {
			_activePlugin = nullptr;
		}
		_backendContext = nullptr;
		_defaultSampler = VK_NULL_HANDLE;
		_descriptorPool = VK_NULL_HANDLE;
		_ctx = nullptr;
		this->_isEnabeld = false;
	}
} // namespace mythril
