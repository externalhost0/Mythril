//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <volk.h>

namespace mythril {
	class CTX;

#ifdef MYTH_ENABLED_IMGUI
	class ImGuiPlugin {
	public:
		void onInit(CTX& ctx, SDL_Window* sdlWindow, VkFormat format);
		void onDestroy();
		inline bool isEnabled() const { return _isEnabeld; }
		inline VkFormat getFormat() const { return _requestedFormat; }
	private:
		CTX* _ctx = nullptr;
		VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
		VkFormat _requestedFormat = VkFormat::VK_FORMAT_UNDEFINED;
		bool _isEnabeld = false;
	};
#endif
}