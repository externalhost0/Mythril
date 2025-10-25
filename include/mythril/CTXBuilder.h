//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "../../lib/CTX.h"

#include <string>
#include <memory>

#include <VkBootstrap.h>

class SDL_Window;

namespace mythril {
	class BasePlugin;

	struct VulkanInfoSpec {
		const char* app_name = "Untitled App";
		const char* engine_name = "Untitled Engine";
	};

	enum class WindowMode { Windowed, Fullscreen };
	struct WindowSpec {
		std::string title = "Untitled Window";
		WindowMode mode = WindowMode::Windowed;
		uint16_t width = 1280;
		uint16_t height = 720;
		bool resizeable = false;
	};

	class CTXBuilder final {
	public:
		CTXBuilder() = default;
		~CTXBuilder() = default;

		CTXBuilder& set_info_spec(const VulkanInfoSpec& spec) {
			this->_vkinfo_spec = spec;
			return *this;
		};
		CTXBuilder& set_window_spec(const WindowSpec& spec) {
			this->_window_spec = spec;
			return *this;
		};
		CTXBuilder& with_ImGui() {
#ifndef MYTH_ENABLED_IMGUI
			ASSERT_MSG(false, "You can not use the ImGui Plugin unless you also enable it in CMake via the options 'MYTH_ENABLE_IMGUI_STANDARD ON' or 'MYTH_ENABLE_IMGUI_DOCKING ON'!");
#endif
			this->_usingImGui = true;
			return *this;
		}

		std::unique_ptr<CTX> build();
	private:
		bool _usingImGui = false;

		VulkanInfoSpec _vkinfo_spec;
		WindowSpec _window_spec;

		void _createVulkanInstance(CTX& ctx, vkb::Instance& vkb_instance_EMPTY) const;
		void _createVulkanPhysDevice(CTX& ctx, vkb::Instance& vkb_instance, vkb::PhysicalDevice& vkb_physdevice_EMPTY) const;
		void _createVulkanLogicalDevice(CTX& ctx, vkb::PhysicalDevice& vkb_physdevice, vkb::Device& vkb_device_EMPTY) const;
		void _createMemoryAllocator(CTX& ctx) const;
		void _createVulkanQueues(CTX& ctx, vkb::Device& vkb_device) const;
		void _createVulkanSurface(CTX& ctx, SDL_Window* sdlWindow) const;
	};
}