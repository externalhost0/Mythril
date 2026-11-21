//
// Created by Hayden Rivas on 10/11/25.
//
#include "../HandleAccess.h"
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

	VkDescriptorSet ImGuiPlugin::getOrCreateImageDescriptor(TextureHandle texHandle, SamplerHandle samplerHandle) {
		ImageDescriptorKey key{
		    .texture = texHandle,
		    .sampler = samplerHandle,
		};
		if (const auto iterator = _imageDescriptors.find(key); iterator != _imageDescriptors.end()) {
			return iterator->second;
		}

		ASSERT_MSG(_ctx, "Mythril ImGui plugin is missing its CTX.");
		const detail::AllocatedTexture& texture = _ctx->view(texHandle);
		ASSERT_MSG(texture.isSampledImage(), "Texture '{}' must have sampled flag!", texture.getDebugName());

		const VkSampler sampler = samplerHandle.valid() ? _ctx->view(samplerHandle).getSampler() : _defaultSampler;
		ScopedImGuiContext context(_backendContext);
		VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(sampler, texture.getImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		_imageDescriptors.insert({key, ds});
		return ds;
	}

	void ImGuiPlugin::removeImageDescriptor(TextureHandle texHandle) {
		std::vector<VkDescriptorSet> descriptors;
		for (auto it = _imageDescriptors.begin(); it != _imageDescriptors.end();) {
			if (it->first.texture == texHandle) {
				descriptors.push_back(it->second);
				it = _imageDescriptors.erase(it);
			} else {
				++it;
			}
		}
		deferImageDescriptorRemoval(std::move(descriptors));
	}

	void ImGuiPlugin::removeImageDescriptors(SamplerHandle samplerHandle) {
		std::vector<VkDescriptorSet> descriptors;
		for (auto it = _imageDescriptors.begin(); it != _imageDescriptors.end();) {
			if (it->first.sampler == samplerHandle) {
				descriptors.push_back(it->second);
				it = _imageDescriptors.erase(it);
			} else {
				++it;
			}
		}
		deferImageDescriptorRemoval(std::move(descriptors));
	}

	void ImGuiPlugin::deferImageDescriptorRemoval(std::vector<VkDescriptorSet> descriptors) {
		if (descriptors.empty()) {
			return;
		}
		ASSERT_MSG(_ctx, "Mythril ImGui plugin is missing its CTX.");
		_ctx->deferTask(std::packaged_task<void()>([this, descriptors = std::move(descriptors)]() {
			ScopedImGuiContext context(_backendContext);
			for (VkDescriptorSet descriptor: descriptors) {
				ImGui_ImplVulkan_RemoveTexture(descriptor);
			}
		}));
	}

	void ImGuiPlugin::clearImageDescriptors() {
		ScopedImGuiContext context(_backendContext);
		for (const auto& [key, descriptor]: _imageDescriptors) {
			ImGui_ImplVulkan_RemoveTexture(descriptor);
		}
		_imageDescriptors.clear();
	}

	void ImGuiPlugin::renderImage(TextureHandle texHandle, SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		ASSERT_MSG(texHandle.valid(), "Texture handle is invalid!");
		ImGuiPlugin* plugin = _activePlugin;
		ASSERT_MSG(plugin && plugin->_ctx && plugin->_isEnabeld, "No active Mythril ImGui plugin is available for ImGui::Image(TextureHandle).");

		ImVec2 size;
		if (image_size.x <= 0 && image_size.y <= 0) {
			const Dimensions dims = plugin->_ctx->view(texHandle).getDimensions();
			size = {static_cast<float>(dims.width), static_cast<float>(dims.height)};
		} else {
			size = image_size;
		}

		VkDescriptorSet im_image_ds = plugin->getOrCreateImageDescriptor(texHandle, samplerHandle);
		ImGui::Image(reinterpret_cast<ImTextureID>(im_image_ds), size, uv0, uv1);
	}
} // namespace mythril
namespace ImGui {
	void Image(mythril::TextureHandle texHandle, mythril::SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		mythril::ImGuiPlugin::renderImage(texHandle, samplerHandle, image_size, uv0, uv1);
	}
	void Image(mythril::TextureHandle texHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texHandle, mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, mythril::TextureViewKey viewKey, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		const mythril::TextureHandle viewHandle = texture.getContext()->getTextureViewHandle(texture.handle(), viewKey);
		Image(viewHandle, mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texture.handle(), mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, const mythril::Sampler& sampler, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texture.handle(), sampler.handle(), image_size, uv0, uv1);
	}
} // namespace ImGui
