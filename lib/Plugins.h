//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

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

		// Called from ImGui::Image free functions in CTX.cpp.
		static void renderImage(TextureHandle texHandle, SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1);

	private:
		struct ImageDescriptorKey {
			TextureHandle texture;
			SamplerHandle sampler;

			bool operator==(const ImageDescriptorKey& other) const {
				return texture == other.texture && sampler == other.sampler;
			}
		};

		struct ImageDescriptorKeyHash {
			std::size_t operator()(const ImageDescriptorKey& key) const noexcept {
				const std::size_t textureHash = std::hash<TextureHandle>()(key.texture);
				const std::size_t samplerHash = std::hash<SamplerHandle>()(key.sampler);
				return textureHash ^ (samplerHash + 0x9e3779b97f4a7c15ull + (textureHash << 6) + (textureHash >> 2));
			}
		};

		VkDescriptorSet getOrCreateImageDescriptor(TextureHandle texHandle, SamplerHandle samplerHandle);
		void removeImageDescriptor(TextureHandle texHandle);
		void removeImageDescriptors(SamplerHandle samplerHandle);
		void deferImageDescriptorRemoval(std::vector<VkDescriptorSet> descriptors);
		void clearImageDescriptors();

		CTX* _ctx = nullptr;
		VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
		VkFormat _requestedFormat = VK_FORMAT_UNDEFINED;
		VkSampler _defaultSampler = VK_NULL_HANDLE;
		ImGuiContext* _backendContext = nullptr;
		std::unordered_map<ImageDescriptorKey, VkDescriptorSet, ImageDescriptorKeyHash> _imageDescriptors;
		std::function<void()> _funcWindowInit;
		std::function<void()> _funcWindowDestroy;
		bool _isEnabeld = false;

		inline static ImGuiPlugin* _activePlugin = nullptr;

		friend class CTX;
	};
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
	class TracyGPUPlugin {
	public:
		void onInit(CTX& ctx);
		void onDestroy();
		TracyVkCtx getTracyVkCtx() { return _tracyVkCtx; }

	private:
		CTX* _ctx = nullptr;
		TracyVkCtx _tracyVkCtx = nullptr;
		VkCommandPool _commandPool = VK_NULL_HANDLE;
		VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;
	};
#endif
} // namespace mythril
