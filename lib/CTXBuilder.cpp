//
// Created by Hayden Rivas on 10/5/25.
//


#include "mythril/CTXBuilder.h"
#include "HelperMacros.h"
#include "CTX.h"
#include "vkutil.h"
#include "Logger.h"
#include "Plugins.h"

#include <volk.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace mythril {
	static constexpr const char* ResolveInstanceExtension_Colorspace(const VkColorSpaceKHR colorSpaceKhr) {
		switch (colorSpaceKhr) {
			case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT:
			case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT:
			case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
			case VK_COLOR_SPACE_BT709_LINEAR_EXT:
			case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
			case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT:
			case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT:
			case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
			case VK_COLOR_SPACE_DOLBYVISION_EXT:
			case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
			case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
			case VK_COLOR_SPACE_HDR10_HLG_EXT:
			case VK_COLOR_SPACE_HDR10_ST2084_EXT:
			case VK_COLOR_SPACE_PASS_THROUGH_EXT:
				return VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
			default: return nullptr;
		}
	}

	struct VulkanInstanceInputs
	{
		const char* appName = nullptr;
		const char* engineName = nullptr;
	};
	static vkb::Instance CreateVulkanInstance(VulkanInstanceInputs inputs, bool enableValidation, std::span<const char*> instance_extensions) {
		vkb::InstanceBuilder instanceBuilder;
		vkb::Result<vkb::Instance> instanceResult = instanceBuilder
				.set_app_name(inputs.appName)
				.set_engine_name(inputs.engineName)
				.require_api_version(VK_API_VERSION_1_3)
				.set_minimum_instance_version(1, 4, 304)
				.request_validation_layers(enableValidation)
				.enable_extensions(instance_extensions)
#ifdef DEBUG
				.use_default_debug_messenger()
#endif
				.build();
		ASSERT_MSG(instanceResult.has_value(), "Failed to create Vulkan instance, Error: {}", instanceResult.error().message());
		vkb::Instance vkb_instance = instanceResult.value();
		volkLoadInstance(vkb_instance.instance);
		return vkb_instance;
	}

	struct VulkanSurfaceInputs
	{
		VkInstance vkInstance = VK_NULL_HANDLE;
		SDL_Window* sdlWindow = nullptr;
	};
	static VkSurfaceKHR CreateVulkanSurface(VulkanSurfaceInputs inputs) {
		ASSERT(inputs.vkInstance != VK_NULL_HANDLE);
		ASSERT(inputs.sdlWindow != nullptr);

		VkSurfaceKHR surface = nullptr;
		const bool surface_success = SDL_Vulkan_CreateSurface(inputs.sdlWindow, inputs.vkInstance, nullptr, &surface);
		ASSERT_MSG(surface_success, "Failed to create Vulkan surface with GLFW");
		return surface;
	}
	struct VulkanPhysicalDeviceInputs {
		vkb::Instance vkbInstance = {};
		VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
	};
	static vkb::PhysicalDevice SelectVulkanPhysicalDevice(VulkanPhysicalDeviceInputs inputs) {
		ASSERT(inputs.vkSurface != VK_NULL_HANDLE);
		ASSERT(inputs.vkbInstance.instance != VK_NULL_HANDLE);

		vkb::PhysicalDeviceSelector physicalDeviceSelector{inputs.vkbInstance};
		vkb::Result<vkb::PhysicalDevice> physicalDeviceResult = physicalDeviceSelector
		.set_minimum_version(1, 3)
		.set_surface(inputs.vkSurface)
		.select();
		ASSERT_MSG(physicalDeviceResult.has_value(), "Failed to select Vulkan Physical Device. Error: {}", physicalDeviceResult.error().message());
		return physicalDeviceResult.value();
	}

	static vkb::Device CreateVulkanLogicalDevice(const vkb::PhysicalDevice& vkbPhysicalDevice, VulkanFeatures& outFeatures, VulkanProperties& outProperties) {
		ASSERT(vkbPhysicalDevice.physical_device != VK_NULL_HANDLE);

		// query supported features
		VkPhysicalDeviceFeatures2 supportedfeatures10 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceVulkan11Features supportedfeatures11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		VkPhysicalDeviceVulkan12Features supportedfeatures12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceVulkan13Features supportedfeatures13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		supportedfeatures10.pNext = &supportedfeatures11;
		supportedfeatures11.pNext = &supportedfeatures12;
		supportedfeatures12.pNext = &supportedfeatures13;
		vkGetPhysicalDeviceFeatures2(vkbPhysicalDevice.physical_device, &supportedfeatures10);
		// query supported properties
		VkPhysicalDeviceProperties2 props10 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		VkPhysicalDeviceVulkan11Properties props11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
		VkPhysicalDeviceVulkan12Properties props12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
		VkPhysicalDeviceVulkan13Properties props13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
		props10.pNext = &props11;
		props11.pNext = &props12;
		props12.pNext = &props13;
		vkGetPhysicalDeviceProperties2(vkbPhysicalDevice.physical_device, &props10);

		// Features that are required for mythril are enabled & disabled

		//vulkan 1.0 features
		VkPhysicalDeviceFeatures2 final10 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		final10.features = supportedfeatures10.features;
		final10.features.samplerAnisotropy = true;
		final10.features.depthClamp = true;
		final10.features.depthBiasClamp = true;
		final10.features.robustBufferAccess = false; // mvk gives us a warning about robust buffer access

		// vulkan 1.1 features
		VkPhysicalDeviceVulkan11Features final11 = supportedfeatures11;
		final11.shaderDrawParameters = true;

		//vulkan 1.2 features
		VkPhysicalDeviceVulkan12Features final12 = supportedfeatures12;
		final12.bufferDeviceAddress = true;
		final12.descriptorIndexing = true;
		final12.timelineSemaphore = true;
		final12.scalarBlockLayout = true;
		final12.uniformAndStorageBuffer8BitAccess = true;
		final12.uniformBufferStandardLayout = true;
		final12.descriptorBindingSampledImageUpdateAfterBind = true;
		final12.descriptorBindingStorageImageUpdateAfterBind = true;
		final12.descriptorBindingUpdateUnusedWhilePending = true;
		final12.descriptorBindingPartiallyBound = true;
		final12.runtimeDescriptorArray = true;
		final12.vulkanMemoryModel = true;

		// vulkan 1.3 features
		VkPhysicalDeviceVulkan13Features final13 = supportedfeatures13;
		final13.robustImageAccess = false;
		final13.synchronization2 = true;
		final13.dynamicRendering = true;
		final13.maintenance4 = true;
		final13.subgroupSizeControl = true;

		// set outFeatures
		outFeatures.features10 = final10.features;
		outFeatures.features11 = final11;
		outFeatures.features12 = final12;
		outFeatures.features13 = final13;
		// set outProperties
		outProperties.props10 = props10.properties;
		outProperties.props11 = props11;
		outProperties.props12 = props12;
		outProperties.props13 = props13;

#ifdef VK_ENABLE_BETA_EXTENSIONS
		VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_subset_features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR,
			.imageViewFormatSwizzle = VK_TRUE
		};
#endif

		vkb::DeviceBuilder logicalDeviceBuilder{vkbPhysicalDevice};
		vkb::Result<vkb::Device> logicalDeviceResult = logicalDeviceBuilder
			.add_pNext(&final13)
			.add_pNext(&final12)
			.add_pNext(&final11)
			.add_pNext(&final10)
#ifdef VK_ENABLE_BETA_EXTENSIONS
			.add_pNext(&portability_subset_features)
#endif
		.build();
		ASSERT_MSG(logicalDeviceResult.has_value(), "Failed to create Vulkan Logical Device. Error: {}", logicalDeviceResult.error().message());
		vkb::Device vkb_device = logicalDeviceResult.value();
		volkLoadDevice(logicalDeviceResult->device);
		return vkb_device;
	}

	struct VulkanMemoryAllocatorInputs
	{
		VkInstance vkInstance;
		VkPhysicalDevice vkPhysicalDevice;
		VkDevice vkDevice;
	};
	static VmaAllocator CreateVulkanMemoryAllocator(const VulkanMemoryAllocatorInputs& inputs) {
		VmaAllocatorCreateInfo allocatorInfo = {
			.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT,
			.physicalDevice = inputs.vkPhysicalDevice,
			.device = inputs.vkDevice,
			.instance = inputs.vkInstance,
			.vulkanApiVersion = VK_API_VERSION_1_3,
		};
		VmaVulkanFunctions vulkanFunctions;
		const VkResult volkimport_result = vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vulkanFunctions);
		ASSERT_MSG(volkimport_result == VK_SUCCESS, "Failed to import vulkan functions from Volk for VMA Allocator create!");
		allocatorInfo.pVulkanFunctions = &vulkanFunctions;
		VmaAllocator vma_allocator = VK_NULL_HANDLE;
		const VkResult allocator_result = vmaCreateAllocator(&allocatorInfo, &vma_allocator);
		ASSERT_MSG(allocator_result == VK_SUCCESS, "Failed to create vma Allocator!");
		return vma_allocator;
	}

	struct VulkanQueueOutputs
	{
		VkQueue vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t graphicsQueueFamilyIndex = -1;
		VkQueue vkPresentQueue = VK_NULL_HANDLE;
		uint32_t presentQueueFamilyIndex = -1;
		VkQueue vkComputeQueue = VK_NULL_HANDLE;
		uint32_t computeQueueFamilyIndex = -1;
	};

	static VulkanQueueOutputs CreateVulkanQueues(const vkb::Device& vkb_device) {
		// out
		VulkanQueueOutputs output{};

		// graphics queue (required)
		auto graphics_queue_result = vkb_device.get_queue(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_result.has_value(), "Failed to get graphics queue. Error: {}", graphics_queue_result.error().message());
		output.vkGraphicsQueue = graphics_queue_result.value();
		// graphics queue index
		auto graphics_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_index_result.has_value(), "Failed to get graphics queue index/family. Error: {}", graphics_queue_index_result.error().message());
		output.graphicsQueueFamilyIndex = graphics_queue_index_result.value();

		// present queue (optional)
		if (auto queue = vkb_device.get_queue(vkb::QueueType::present); queue.has_value()) {
			output.vkPresentQueue = queue.value();
			if (auto index = vkb_device.get_queue_index(vkb::QueueType::present); index.has_value()) {
				output.presentQueueFamilyIndex = index.value();
			}
		} else {
			LOG_SYSTEM(LogType::Warning, "Could not retrieve a present queue.");
		}

		// compute queue (optional)
		if (auto queue = vkb_device.get_queue(vkb::QueueType::compute); queue.has_value()) {
			output.vkComputeQueue = queue.value();
			if (auto index = vkb_device.get_queue_index(vkb::QueueType::compute); index.has_value()) {
				output.computeQueueFamilyIndex = index.value();
			}
		} else {
			LOG_SYSTEM(LogType::Warning, "Could not retrieve a compute queue.");
		}
		return output;
	}

	std::unique_ptr<CTX> CTXBuilder::build() {
		MYTH_PROFILER_ZONE("CTXBuilder::build", MYTH_PROFILER_COLOR_CREATE);
		// mythril should only provide graphics related things
		// fixme as this means that we have no audio
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ASSERT_MSG(true, "SDL could not be initialized!");
		}
		Window window;
		window.create(this->_windowSpec);

		const VkResult volk_result = volkInitialize();
		ASSERT_MSG(volk_result == VK_SUCCESS, "Volk failed to initialize!");

		// fill in user instance extensions & resolve based on cfg
		std::vector optional_instance_extensions(std::begin(this->_vulkanCfg.instanceExtensions), std::end(this->_vulkanCfg.instanceExtensions));
		// if (const auto extension_name = ResolveInstanceExtension_Colorspace(this->_swapchain_spec.colorSpace))
		// 	optional_instance_extensions.push_back(extension_name);

		vkb::Instance vkb_instance = CreateVulkanInstance({
			.appName = this->_vulkanCfg.app_name,
			.engineName = this->_vulkanCfg.engine_name
		}, this->_vulkanCfg.enableValidation, optional_instance_extensions);

		VkSurfaceKHR vk_surface = CreateVulkanSurface({
			.vkInstance = vkb_instance.instance,
			.sdlWindow = window._sdlWindow
		});

		vkb::PhysicalDevice vkb_physical_device = SelectVulkanPhysicalDevice({
			.vkbInstance = vkb_instance,
			.vkSurface = vk_surface
		});

		// fill in user device extensions & resolve based on cfg
		std::vector optional_device_extensions(std::begin(this->_vulkanCfg.deviceExtensions), std::end(this->_vulkanCfg.deviceExtensions));
		// if (this->_swapchain_spec.colorSpace == VK_COLOR_SPACE_DISPLAY_NATIVE_AMD)
		// 	optional_device_extensions.push_back(VK_AMD_DISPLAY_NATIVE_HDR_EXTENSION_NAME);
		if (this->_usingTracyGPU)
			optional_device_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

		// do some things to the physical device before creation
		// we dont have to pass anything like this into vkDevice creation because its all stored in vkb::PhysicalDevice
		for (size_t i = 0; i < optional_device_extensions.size(); i++) {
			if (vkb_physical_device.enable_extension_if_present(optional_device_extensions[i]))
				LOG_SYSTEM(LogType::Info, "Successfully enabled requested extension: '{}'", optional_device_extensions[i]);
			else
				LOG_SYSTEM(LogType::Warning, "Unsupported extension requested: '{}'", optional_device_extensions[i]);
		}

		VulkanFeatures features{};
		VulkanProperties properties{};
		// sets features & properties
		vkb::Device vkb_device = CreateVulkanLogicalDevice(vkb_physical_device, features, properties);
		VmaAllocator vma_allocator = CreateVulkanMemoryAllocator({
			.vkInstance = vkb_instance.instance,
			.vkPhysicalDevice = vkb_physical_device.physical_device,
			.vkDevice = vkb_device.device
		});
		VulkanQueueOutputs queue_output = CreateVulkanQueues(vkb_device);

		// most of the setup we need to worry about is done in the constructor
		// this is the same thing as make_unique, ignore the warninge
		auto ctx = std::unique_ptr<CTX>(new CTX());
		// manually transfer the values recieved previously to ctx

		// important handles
		ctx->_vkInstance = vkb_instance.instance;
		ctx->_vkDebugMessenger = vkb_instance.debug_messenger;
		ctx->_vkSurfaceKHR = vk_surface;
		ctx->_vkPhysicalDevice = vkb_physical_device.physical_device;
		ctx->_vkDevice = vkb_device.device;
		ctx->_vmaAllocator = vma_allocator;
		// info
		ctx->_featuresVulkan = features;
		ctx->_propertiesVulkan = properties;

		auto exts = vkb_physical_device.get_extensions();
		ctx->_enabledDeviceExtensionNames.clear();
		for (const auto& ext : exts) {
			ctx->_enabledDeviceExtensionNames.insert(ext);
		}
		// queues
		ctx->_vkGraphicsQueue = queue_output.vkGraphicsQueue;
		ctx->_graphicsQueueFamilyIndex = queue_output.graphicsQueueFamilyIndex;
		ctx->_vkPresentQueue = queue_output.vkPresentQueue;
		ctx->_presentQueueFamilyIndex = queue_output.presentQueueFamilyIndex;
		ctx->_vkComputeQueue = queue_output.vkComputeQueue;
		ctx->_computeQueueFamilyIndex = queue_output.computeQueueFamilyIndex;

		// our window
		ctx->_window = window;
		// defered initialization of the CTX instance
		ctx->construct();
		if (this->_requestedSwapchain) {
			if (this->_swapchainSpec.width == 0 && this->_swapchainSpec.height == 0) {
					// swapchain must be built after default texture has been made
					// or else the fallback texture is the swapchain's texture
					const auto [width, height] = ctx->getWindow().getFramebufferSize();
					this->_swapchainSpec.width = width;
					this->_swapchainSpec.height = height;
			}
			ctx->createSwapchain(this->_swapchainSpec);
		}

		for (const auto& path : this->_slangCfg.searchpaths) {
			ctx->_slangCompiler.addSearchPath(path);
		}
		// now we can build plugins!
#ifdef MYTH_ENABLED_IMGUI
		if (_usingImGui) ctx->_imguiPlugin.onInit(*ctx, ctx->_window._getSDLwindow(), this->_imguiSpec.format);
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		if (_usingTracyGPU) ctx->_tracyPlugin.onInit(*ctx);
#endif
		return ctx;
		MYTH_PROFILER_ZONE_END();
	}
}