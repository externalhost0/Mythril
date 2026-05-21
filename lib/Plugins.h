//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <volk.h>

#include "mythril/ObjectHandles.h"
#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
#include <tracy/TracyVulkan.hpp>
#endif

namespace mythril {
	struct ImGuiPluginSpec;
	class CTX;

#ifdef MYTH_ENABLED_IMGUI
	class ImGuiPlugin {
	public:
		void onInit(CTX& ctx, ImGuiPluginSpec spec);
		void onDestroy();
		inline bool isEnabled() const { return _isEnabeld; }
		inline VkFormat getFormat() const { return _requestedFormat; }

		// called from ImGui::Image free functions in CTX.cpp — ImGuiPlugin is a CTX friend so it can reach private view()
		static void renderImage(CTX& ctx, VkSampler defaultSampler, TextureHandle texHandle, SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1);

	private:
		CTX* _ctx = nullptr;
		VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
		VkFormat _requestedFormat = VK_FORMAT_UNDEFINED;
		std::function<void()> _funcWindowInit;
		std::function<void()> _funcWindowDestroy;
		bool _isEnabeld = false;
	};
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
	class TracyPlugin {
	public:
		void onInit(CTX& ctx);
		void onDestroy();
		bool isEnabled() const { return _isEnabled; }
		TracyVkCtx getTracyVkCtx() { return _tracyVkCtx; }

	private:
		CTX* _ctx = nullptr;
		TracyVkCtx _tracyVkCtx = nullptr;
		VkCommandPool _commandPool = VK_NULL_HANDLE;
		VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;
		bool _isEnabled = false;
	};
#endif
} // namespace mythril
