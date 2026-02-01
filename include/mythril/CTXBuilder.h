//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "../../lib/CTX.h"

#include <string>
#include <memory>
#include <span>

struct SDL_Window;

namespace mythril {
	static constexpr uint32_t kMaxUserExtensions = 32;

	struct Version
	{
		uint8_t major = 0;
		uint8_t minor = 0;
		uint8_t patch = 0;
		bool isPreRelease() const {
			return major > 0;
		}
		uint32_t getVKVersion() const {
			return VK_MAKE_VERSION(major, minor, patch);
		}
	};
	// there are other options we could place here, however CTXBuilder fields should be one time not reloadable
	// so swapchain options shouldnt be here as they can be reloaded during runtime easily
	struct VulkanCfg
	{
		const char* app_name = "Untitled App";
		const char* engine_name = "Untitled Engine";
		Version app_version = {0, 0, 1};
		Version engine_version = {0, 0, 1};

		bool enableValidation = true;
		// implement this later
		// bool enableHeadless = false;
		std::vector<const char*> instanceExtensions = {};
		std::vector<const char*> deviceExtensions   = {};
		void* deviceExtensionFeatureChain = {};
	};
	struct SlangCfg
	{
		SlangMatrixLayoutMode matrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
		std::span<const char*> searchpaths = {};
		std::span<slang::CompilerOptionEntry> compilerOptions = {};
	};

	struct VulkanInfoSpec {
		const char* app_name = "Untitled App";
		const char* engine_name = "Untitled Engine";
		bool enableValidation = true;
		std::span<const char*> instanceExtensions = {};
		std::span<const char*> deviceExtensions   = {};
	};

	enum class WindowMode { Windowed, Fullscreen, Headless };
	struct WindowSpec
	{
		std::string title = "Untitled Window";
		WindowMode mode = WindowMode::Windowed;
		uint16_t width = 1280;
		uint16_t height = 720;
		bool resizeable = false;
	};


#ifdef MYTH_ENABLED_IMGUI
	struct ImGuiPluginSpec {
		VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	};
#endif

	// factory for CTX
	class CTXBuilder final {
	public:
		CTXBuilder() = default;
		~CTXBuilder() = default;

		CTXBuilder& set_vulkan_cfg(const VulkanCfg& cfg) {
			this->_vulkanCfg = cfg;
			return *this;
		}
		CTXBuilder& set_slang_cfg(const SlangCfg& cfg) {
			this->_slangCfg = cfg;
			return *this;
		}
		CTXBuilder& set_window_spec(const WindowSpec& spec) {
			this->_windowSpec = spec;
			return *this;
		}
		// can resolve width & height from window
		CTXBuilder& with_default_swapchain(const SwapchainSpec& spec = {}) {
			_swapchainSpec = spec;
			_requestedSwapchain = true;
			return *this;
		}

#ifdef MYTH_ENABLED_IMGUI
		CTXBuilder& with_ImGui(ImGuiPluginSpec spec = {}) {
			this->_usingImGui = true;
			this->_imguiSpec = spec;
			return *this;
		}
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		CTXBuilder& with_TracyGPU() {
			this->_usingTracyGPU = true;
			return *this;
		}
#endif

		[[nodiscard]] std::unique_ptr<CTX> build();
	private:
		bool _usingImGui = false;
		bool _usingTracyGPU = false;
		bool _requestedSwapchain = false;
		// all specs & cfgs use default values
		// cfgs are used to resolve data
		// specs are passed straight into the object
		VulkanCfg _vulkanCfg{};
		SlangCfg _slangCfg{};
		SwapchainSpec _swapchainSpec{};
		WindowSpec _windowSpec{};
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPluginSpec _imguiSpec{};
#endif
	};
}