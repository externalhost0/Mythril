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

	struct VulkanInfoSpec {
		const char* app_name = "Untitled App";
		const char* engine_name = "Untitled Engine";
	};

	enum class WindowMode { Windowed, Fullscreen, Headless };
	struct WindowSpec {
		std::string title = "Untitled Window";
		WindowMode mode = WindowMode::Windowed;
		uint16_t width = 1280;
		uint16_t height = 720;
		bool resizeable = false;
	};

	struct SwapchainSpec {
		VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
		VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
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
		CTXBuilder& set_swapchain_spec(const SwapchainSpec& spec) {
			this->_swapchain_spec = spec;
			return *this;
		}
		CTXBuilder& set_shader_search_paths(std::initializer_list<const char*> searchPaths) {
			_searchpaths.clear();
			for (auto path : searchPaths) {
				_searchpaths.emplace_back(path);
			}
			return *this;
		}
		CTXBuilder& set_shader_search_paths(const std::vector<const char*>& searchPaths) {
			for (auto path : searchPaths) {
				_searchpaths.emplace_back(path);
			}
			return *this;
		}
		CTXBuilder& with_ImGui() {
#ifndef MYTH_ENABLED_IMGUI
			ASSERT_MSG(false, "You can not use the ImGui Plugin unless you also enable it in CMake via the options 'MYTH_ENABLE_IMGUI_STANDARD=ON' or 'MYTH_ENABLE_IMGUI_DOCKING=ON'!");
#endif
			this->_usingImGui = true;
			return *this;
		}

		std::unique_ptr<CTX> build();
	private:
		bool _usingImGui = false;

		std::vector<std::string> _searchpaths = {};
		// all specs use default values
		VulkanInfoSpec _vkinfo_spec{};
		WindowSpec _window_spec{};
		SwapchainSpec _swapchain_spec{};
	};
}