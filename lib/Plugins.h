//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <volk.h>

namespace mythril {
	class CTX;

	class BasePlugin {
	public:
		virtual ~BasePlugin() = default;
		virtual void onInit(CTX& ctx, SDL_Window* sdlWindow) {};
		virtual void onDispose() {};
	};

	class ImGuiPlugin : public BasePlugin {
	public:
		void onInit(CTX& ctx, SDL_Window * sdlWindow) override;
		void onDispose() override;
	private:
		CTX* _ctx;
		VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
	};
}