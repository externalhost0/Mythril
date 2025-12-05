//
// Created by Hayden Rivas on 10/5/25.
//


#include "mythril/CTXBuilder.h"
#include "HelperMacros.h"
#include "CTX.h"
#include "Plugins.h"
#include "vkutil.h"
#include "Logger.h"

#include <VkBootstrap.h>

#include <volk.h>
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace mythril {
	struct VulkanInstanceInputs {
		const char* appName = nullptr;
		const char* engineName = nullptr;
	};
	static vkb::Instance CreateVulkanInstance(VulkanInstanceInputs inputs) {
		vkb::InstanceBuilder instanceBuilder;
		vkb::Result<vkb::Instance> instanceResult = instanceBuilder
				.set_app_name(inputs.appName)
				.set_engine_name(inputs.engineName)
				.require_api_version(VK_API_VERSION_1_3)
				.set_minimum_instance_version(1, 4, 304)
				.request_validation_layers()
#ifdef DEBUG
				.use_default_debug_messenger()
#endif
				.build();
		ASSERT_MSG(instanceResult.has_value(), "Failed to create Vulkan instance, Error: {}", instanceResult.error().message());
		vkb::Instance vkb_instance = instanceResult.value();
		volkLoadInstance(vkb_instance.instance);
		return vkb_instance;
	}
	struct VulkanSurfaceInputs {
		VkInstance vkInstance = VK_NULL_HANDLE;
		SDL_Window* sdlWindow = nullptr;
	};
	static VkSurfaceKHR CreateVulkanSurface(VulkanSurfaceInputs inputs) {
		ASSERT(inputs.vkInstance != VK_NULL_HANDLE);
		ASSERT(inputs.sdlWindow != nullptr);

		VkSurfaceKHR surface = nullptr;
		bool surface_success = SDL_Vulkan_CreateSurface(inputs.sdlWindow, inputs.vkInstance, nullptr, &surface);
		ASSERT_MSG(surface_success, "Failed to create Vulkan surface with GLFW");
		return surface;
	}
	struct VulkanPhysicalDeviceInputs {
		vkb::Instance vkbInstance = {};
		VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
	};
	static vkb::PhysicalDevice CreateVulkanPhysicalDevice(VulkanPhysicalDeviceInputs inputs, std::span<const char*> additional_extensions) {
		ASSERT(inputs.vkSurface != VK_NULL_HANDLE);
		ASSERT(inputs.vkbInstance.instance != VK_NULL_HANDLE);

		// Physical Device
		// vulkan 1.3 features
		VkPhysicalDeviceVulkan13Features features13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		features13.synchronization2 = true;
		features13.dynamicRendering = true;
		features13.subgroupSizeControl = true;
		//vulkan 1.2 features
		VkPhysicalDeviceVulkan12Features features12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		features12.shaderInt8 = true;
		features12.shaderFloat16 = true;
		features12.bufferDeviceAddress = true;
		features12.descriptorIndexing = true;
		features12.timelineSemaphore = true;
		features12.scalarBlockLayout = true;
		features12.uniformAndStorageBuffer8BitAccess = true;
		features12.uniformBufferStandardLayout = true;
		features12.shaderSampledImageArrayNonUniformIndexing = true;
		features12.shaderStorageImageArrayNonUniformIndexing = true;
		features12.descriptorBindingSampledImageUpdateAfterBind = true;
		features12.descriptorBindingStorageImageUpdateAfterBind = true;
		features12.descriptorBindingUpdateUnusedWhilePending = true;
		features12.descriptorBindingVariableDescriptorCount = true;
		features12.descriptorBindingPartiallyBound = true;
		features12.runtimeDescriptorArray = true;
		// for spirv, slang compiler capability
		features12.vulkanMemoryModel = true;
		features12.vulkanMemoryModelDeviceScope = true;
		// vulkan 1.1 features
		VkPhysicalDeviceVulkan11Features features11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
		features11.storageBuffer16BitAccess = true;
		features11.shaderDrawParameters = true;
		//vulkan 1.0 features
		VkPhysicalDeviceFeatures features = {};
		features.vertexPipelineStoresAndAtomics = true;
		features.fragmentStoresAndAtomics = true;
		features.fillModeNonSolid = true;
		features.independentBlend = true;
		features.shaderInt64 = true;
		features.multiDrawIndirect = true;
		features.drawIndirectFirstInstance = true;
		features.samplerAnisotropy = true;
		features.shaderImageGatherExtended = true;
		features.samplerAnisotropy = true;

		vkb::PhysicalDeviceSelector physicalDeviceSelector{inputs.vkbInstance};
		vkb::Result<vkb::PhysicalDevice> physicalDeviceResult = physicalDeviceSelector
				.set_minimum_version(1, 2)
				.set_surface(inputs.vkSurface)
				.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
				.add_required_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
				.add_required_extension(VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME)

				.add_required_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)
				.add_required_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME)
				.add_required_extension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME)
				.add_required_extension(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME)
				.add_required_extension(VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME)
				.add_required_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)
				.add_required_extension(VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME)

				.add_required_extensions(additional_extensions)

				.set_required_features_13(features13)
				.set_required_features_12(features12)
				.set_required_features_11(features11)
				.set_required_features(features)
				.select();
		ASSERT_MSG(physicalDeviceResult.has_value(), "Failed to select Vulkan Physical Device. Error: {}", physicalDeviceResult.error().message());
		return physicalDeviceResult.value();
	}

	static vkb::Device CreateVulkanLogicalDevice(const vkb::PhysicalDevice& vkbPhysicalDevice) {
		ASSERT(vkbPhysicalDevice.physical_device != VK_NULL_HANDLE);

		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic_state_feature = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
		dynamic_state_feature.pNext = nullptr;
		dynamic_state_feature.extendedDynamicState = VK_TRUE;

		VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamic_state_feature_2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
		dynamic_state_feature_2.pNext = nullptr;
		dynamic_state_feature_2.extendedDynamicState2 = VK_TRUE;

		vkb::DeviceBuilder logicalDeviceBuilder{vkbPhysicalDevice};
		vkb::Result<vkb::Device> logicalDeviceResult = logicalDeviceBuilder
				.add_pNext(&dynamic_state_feature)
				.add_pNext(&dynamic_state_feature_2)
				.build();

		ASSERT_MSG(logicalDeviceResult.has_value(), "Failed to create Vulkan Logical Device. Error: {}", logicalDeviceResult.error().message());
		vkb::Device vkb_device = logicalDeviceResult.value();
		volkLoadDevice(logicalDeviceResult->device);
		// alias KHR and EXT functions
#if defined(__APPLE__)
		vkCmdBeginRendering = vkCmdBeginRenderingKHR;
		vkCmdEndRendering = vkCmdEndRenderingKHR;
		vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
		vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
		vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
		vkCmdSetDepthBiasEnable = vkCmdSetDepthBiasEnableEXT;
		vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
		vkQueueSubmit2 = vkQueueSubmit2KHR;
#endif
		return vkb_device;
	}
	struct VulkanMemoryAllocatorInputs {
		VkInstance vkInstance;
		VkPhysicalDevice vkPhysicalDevice;
		VkDevice vkLogicalDevice;
	};
	static VmaAllocator CreateVulkanMemoryAllocator(VulkanMemoryAllocatorInputs inputs) {
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = inputs.vkPhysicalDevice;
		allocatorInfo.device = inputs.vkLogicalDevice;
		allocatorInfo.instance = inputs.vkInstance;
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
		VmaVulkanFunctions vulkanFunctions;
		VkResult volkimport_result = vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vulkanFunctions);
		ASSERT_MSG(volkimport_result == VK_SUCCESS, "Failed to import vulkan functions from Volk for VMA Allocator create!");
		allocatorInfo.pVulkanFunctions = &vulkanFunctions;
		VmaAllocator vma_allocator = VK_NULL_HANDLE;
		VkResult allocator_result = vmaCreateAllocator(&allocatorInfo, &vma_allocator);
		ASSERT_MSG(allocator_result == VK_SUCCESS, "Failed to create vma Allocator!");
		return vma_allocator;
	}

	struct VulkanQueueOutputs {
		VkQueue vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t graphicsQueueFamilyIndex = -1;
		VkQueue vkPresentQueue = VK_NULL_HANDLE;
		uint32_t presentQueueFamilyIndex = -1;
	};
	static VulkanQueueOutputs CreateVulkanQueues(const vkb::Device& vkb_device) {
		// result
		VulkanQueueOutputs output{};

		// graphics queue
		vkb::Result<VkQueue_T*> graphics_queue_result = vkb_device.get_queue(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_result.has_value(), "Failed to get graphics queue. Error: {}", graphics_queue_result.error().message());
		output.vkGraphicsQueue = graphics_queue_result.value();

		// graphics queue index
		vkb::Result<uint32_t> graphics_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_index_result.has_value(), "Failed to get graphics queue index/family. Error: {}", graphics_queue_index_result.error().message());
		output.graphicsQueueFamilyIndex = graphics_queue_index_result.value();

		// present queue
		vkb::Result<VkQueue_T*> present_queue_result = vkb_device.get_queue(vkb::QueueType::present);
		ASSERT_MSG(present_queue_result.has_value(), "Failed to get present queue. Error: {}", present_queue_result.error().message());
		output.vkPresentQueue = present_queue_result.value();

		// present queue index
		vkb::Result<uint32_t> present_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::present);
		ASSERT_MSG(present_queue_index_result.has_value(), "Failed to get present queue index/family. Error: {}", present_queue_index_result.error().message());
		output.presentQueueFamilyIndex = present_queue_index_result.value();

		return output;
	}

	const char* ResolveAdditional_SwapchainColorSpace(VkColorSpaceKHR colorSpaceKhr) {
		switch (colorSpaceKhr) {
			case VkColorSpaceKHR::VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return nullptr;
			case VkColorSpaceKHR::VK_COLOR_SPACE_DISPLAY_NATIVE_AMD: return VK_AMD_DISPLAY_NATIVE_HDR_EXTENSION_NAME;
			default: return VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
		}
	}

	std::unique_ptr<CTX> CTXBuilder::build() {
		// mythril should only provide graphics related things
		// fixme as this means that we have no audio
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ASSERT_MSG(true, "SDL could not be initialized!");
		}
		Window window;
		window.create(this->_window_spec);

		VkResult volk_result = volkInitialize();
		ASSERT_MSG(volk_result == VK_SUCCESS, "Volk failed to initialize!");

		vkb::Instance vkb_instance = CreateVulkanInstance({
			.appName = this->_vkinfo_spec.app_name,
			.engineName = this->_vkinfo_spec.engine_name
		});
		VkSurfaceKHR vk_surface = CreateVulkanSurface({
			.vkInstance = vkb_instance.instance,
			.sdlWindow = window._sdlWindow
		});

		std::vector<const char*> additional_extensions = {};
		const char* swapchain_extension = ResolveAdditional_SwapchainColorSpace(this->_swapchain_spec.colorSpace);
		if (swapchain_extension) additional_extensions.push_back(swapchain_extension);

		vkb::PhysicalDevice vkb_physical_device = CreateVulkanPhysicalDevice({
			.vkbInstance = vkb_instance,
			.vkSurface = vk_surface
		}, additional_extensions);
		vkb::Device vkb_device = CreateVulkanLogicalDevice(vkb_physical_device);
		VmaAllocator vma_allocator = CreateVulkanMemoryAllocator({
			.vkInstance = vkb_instance.instance,
			.vkPhysicalDevice = vkb_physical_device.physical_device,
			.vkLogicalDevice = vkb_device.device
		});
		VulkanQueueOutputs queue_output = CreateVulkanQueues(vkb_device);

		// most of the setup we need to worry about is done in the constructor
		std::unique_ptr<CTX> ctx = std::make_unique<CTX>();
		// manually transfer the values to ctx
		ctx->_vkInstance = vkb_instance.instance;
		ctx->_vkDebugMessenger = vkb_instance.debug_messenger;
		ctx->_vkSurfaceKHR = vk_surface;
		ctx->_vkPhysicalDevice = vkb_physical_device.physical_device;
		ctx->_vkDevice = vkb_device.device;
		ctx->_vmaAllocator = vma_allocator;

		// get physical device properties
		VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		VkPhysicalDeviceVulkan13Properties props13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
		VkPhysicalDeviceVulkan12Properties props12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
		VkPhysicalDeviceVulkan11Properties props11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
		props.pNext = &props13;
		props13.pNext = &props12;
		props12.pNext = &props11;
		vkGetPhysicalDeviceProperties2(ctx->_vkPhysicalDevice, &props);
		ctx->_vkPhysDeviceProperties = vkb_physical_device.properties;
		ctx->_vkPhysDeviceVulkan13Properties = props13;
		ctx->_vkPhysDeviceVulkan12Properties = props12;
		ctx->_vkPhysDeviceVulkan11Properties = props11;

		ctx->_vkGraphicsQueue = queue_output.vkGraphicsQueue;
		ctx->_graphicsQueueFamilyIndex = queue_output.graphicsQueueFamilyIndex;
		ctx->_vkPresentQueue = queue_output.vkPresentQueue;
		ctx->_presentQueueFamilyIndex = queue_output.presentQueueFamilyIndex;

		ctx->_window = window;
		SwapchainArgs args = {
				.format = this->_swapchain_spec.format,
				.colorSpace = this->_swapchain_spec.colorSpace,
				.presentMode = this->_swapchain_spec.presentMode
		};
		ctx->construct(args);

		for (auto& path : this->_searchpaths) {
			ctx->_slangCompiler.addSearchPath(path);
		}
		// now we can build plugins!
#ifdef MYTH_ENABLED_IMGUI
		if (_usingImGui) {
			ImGuiPlugin imgui;
			imgui.onInit(*ctx, ctx->getWindow()._sdlWindow);
			ctx->_plugins.emplace_back(std::make_unique<ImGuiPlugin>(imgui));
		}
#endif

		return ctx;
	}


}