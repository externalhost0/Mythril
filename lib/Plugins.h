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
		virtual void onInit(CTX& ctx) {};
		virtual void onDispose() {};
	private:
	};
	class ImGuiPlugin : public BasePlugin {
	public:
		void onInit(CTX& ctx) override;
		void onDispose() override;
	private:
		CTX* _ctx;
		VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
	};
}