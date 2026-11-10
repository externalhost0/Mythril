//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once


#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>


#include "SwapchainSpec.h"

namespace slang {
	struct CompilerOptionEntry;
}

namespace mythril {
	class CTX;
	struct CtxDeleter {
		void operator()(const CTX* ctx) const noexcept;
	};
	using CtxPtr = std::unique_ptr<CTX, CtxDeleter>;

	struct Version
	{
		uint8_t major = 0;
		uint8_t minor = 0;
		uint8_t patch = 0;
		bool isPreRelease() const {
			return major < 1;
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
		// instance extension names
		std::vector<const char*> instanceExtensions = {};
		// device extension names
		std::vector<const char*> deviceExtensions   = {};
		// made just for int64 feature supported
		VkPhysicalDeviceFeatures additionalDeviceFeatures = {};
		// device extension feautures, will not be handled if unsupported!
		void* deviceExtensionFeatureChain = {};
	};

	struct SlangCfg
	{
		enum class MatrixMode {
			RowMajor,
			ColMajor
		};
		MatrixMode matrixLayoutMode = MatrixMode::ColMajor;
		std::vector<std::string> searchpaths = {};
		slang::CompilerOptionEntry* compilerOptionsData = nullptr;
		size_t compilerOptionsCount = 0;
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


	struct SurfaceSpec {
		std::span<const char* const> instanceExtensions = {};
		std::function<VkSurfaceKHR(VkInstance)> createFunc;
		std::function<void(VkInstance, VkSurfaceKHR)> destroyFunc;
	};

#ifdef MYTH_ENABLED_IMGUI
	struct ImGuiPluginSpec {
		VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
		std::function<void()> windowInitFunction;
		std::function<void()> windowDestroyFunction;
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
		CTXBuilder& set_window_surface(const SurfaceSpec& spec) {
			if (!spec.createFunc)
				throw std::runtime_error("SurfaceSpec: createFunc is required");
			if (!spec.destroyFunc)
				throw std::runtime_error("SurfaceSpec: destroyFunc is required");

			this->_surfaceSpec = spec;
			return *this;
		}
		// can resolve width & height from window
		CTXBuilder& with_default_swapchain(const SwapchainSpec& spec = {}) {
			_swapchainSpec = spec;
			_requestedSwapchain = true;
			return *this;
		}

#ifdef MYTH_ENABLED_IMGUI
		CTXBuilder& with_ImGui(const ImGuiPluginSpec& spec) {
			if (!spec.windowInitFunction)
				throw std::runtime_error("ImGuiPluginSpec: windowInitFunction is required");
			if (!spec.windowDestroyFunction)
				throw std::runtime_error("ImGuiPluginSpec: windowDestroyFunction is required");

			this->_usingImGui = true;
			this->_imguiSpec = spec;
			return *this;
		}
#endif

		[[nodiscard]] CtxPtr build();
	private:
		bool _usingImGui = false;
		bool _requestedSwapchain = false;
		// all specs & cfgs use default values
		// cfgs are used to resolve data
		// specs are passed straight into the object
		VulkanCfg _vulkanCfg{};
		SlangCfg _slangCfg{};
		SwapchainSpec _swapchainSpec{};
		SurfaceSpec _surfaceSpec{};
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPluginSpec _imguiSpec{};
#endif
	};
}