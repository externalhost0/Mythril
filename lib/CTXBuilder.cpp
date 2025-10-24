//
// Created by Hayden Rivas on 10/5/25.
//


#include "mythril/CTXBuilder.h"
#include "HelperMacros.h"
#include "CTX.h"
#include "Plugins.h"
#include "vkutil.h"
#include "Logger.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace mythril {
	std::unique_ptr<CTX> CTXBuilder::build() {
		std::unique_ptr<CTX> ctx = std::make_unique<CTX>();

		// mythril should only provide graphics related things
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ASSERT_MSG(true, "SDL could not be initialized!");
		}

		ctx->_window.create(_window_spec);

		// volk init
		VkResult volk_result = volkInitialize();
		ASSERT_MSG(volk_result == VK_SUCCESS, "Volk failed to initialize!");

		// build vulkan
		vkb::Instance tempVKBInstance;
		_createVulkanInstance(*ctx, tempVKBInstance);
		_createVulkanSurface(*ctx, ctx->_window._getSDLwindow());
		vkb::PhysicalDevice tempVKBPhysDevice;
		_createVulkanPhysDevice(*ctx, tempVKBInstance, tempVKBPhysDevice);
		vkb::Device tempVKBDevice;
		_createVulkanLogicalDevice(*ctx, tempVKBPhysDevice, tempVKBDevice);
		_createMemoryAllocator(*ctx);
		_createVulkanQueues(*ctx, tempVKBDevice);
		ctx->_imm = std::make_unique<ImmediateCommands>(ctx->_vkDevice, ctx->_graphicsQueueFamilyIndex);
		ctx->_staging = std::make_unique<StagingDevice>(*ctx);
		// DEFAULT VULKAN OBJECTS
		{
			// pattern xor
			const uint32_t texWidth = 256;
			const uint32_t texHeight = 256;
			std::vector<uint32_t> pixels(texWidth * texHeight);
			for (uint32_t y = 0; y != texHeight; y++) {
				for (uint32_t x = 0; x != texWidth; x++) {
					pixels[y * texWidth + x] =
							0xFF000000 + ((x ^ y) << 16) + ((x ^ y) << 8) + (x ^ y);
				}
			}
			// now take the pattern and create the first texture, the default and fallback texture for missing textures
			ctx->_dummyTextureHandle = ctx->createTexture({
				.dimension = {texWidth, texHeight},
				.usage = TextureUsageBits::TextureUsageBits_Sampled | TextureUsageBits::TextureUsageBits_Storage,
				.format = VK_FORMAT_R8G8B8A8_UNORM,
				.data = pixels.data(),
				.debugName = "Dummy Texture"
			});
			ctx->_linearSamplerHandle = ctx->createSampler({
				.magFilter = SamplerFilter::Linear,
				.minFilter = SamplerFilter::Linear,
				.wrapU = SamplerWrap::Clamp,
				.wrapV = SamplerWrap::Clamp,
				.wrapW = SamplerWrap::Clamp,
				.mipMap = SamplerMip::Disabled,
				.debugName = "Linear Sampler"
			});
			ctx->_nearestSamplerHandle = ctx->createSampler({
				.magFilter = SamplerFilter::Nearest,
				.minFilter = SamplerFilter::Nearest,
				.wrapU = SamplerWrap::Clamp,
				.wrapV = SamplerWrap::Clamp,
				.wrapW = SamplerWrap::Clamp,
				.mipMap = SamplerMip::Disabled,
				.debugName = "Nearest Sampler"
			});
		}
		// swapchain must be built after default texture has been made
		// or else the fallback texture is the swapchain's texture
		VkExtent2D framebufferSize = ctx->getWindow().getFramebufferSize();
		ctx->_swapchain = std::make_unique<Swapchain>(*ctx, framebufferSize.width, framebufferSize.height);
		// timeline semaphore is closely kept to vulkan swapchain
		ctx->_timelineSemaphore = vkutil::CreateTimelineSemaphore(ctx->_vkDevice, ctx->_swapchain->getNumOfSwapchainImages() - 1);
		ctx->growDescriptorPool(ctx->_currentMaxTextureCount, ctx->_currentMaxSamplerCount);
		// now we can build plugins!
		if (_usingImGui) {
			ImGuiPlugin imgui;
			imgui.onInit(*ctx, ctx->getWindow()._sdlWindow);
			ctx->_plugins.emplace_back(std::make_unique<ImGuiPlugin>(imgui));
		}
		return ctx;
	}
	void CTXBuilder::_createVulkanInstance(CTX& ctx, vkb::Instance& vkb_instance) const {
		// Instance
		vkb::InstanceBuilder instanceBuilder;
		vkb::Result<vkb::Instance> instanceResult = instanceBuilder
				.set_app_name(_vkinfo_spec.app_name)
				.set_engine_name(_vkinfo_spec.engine_name)
				.require_api_version(VK_API_VERSION_1_3)
				.request_validation_layers()
#ifdef DEBUG
				.use_default_debug_messenger()
#endif
				.build();
		ASSERT_MSG(instanceResult.has_value(), "Failed to create Vulkan instance, Error: {}", instanceResult.error().message());
		vkb_instance = instanceResult.value();
		ctx._vkInstance = instanceResult->instance;
#ifdef DEBUG
		ctx._vkDebugMessenger = instanceResult->debug_messenger;
#endif
		volkLoadInstance(ctx._vkInstance);

		// Instance Info (Optional)
		vkb::Result<vkb::SystemInfo> systemInfoResult = vkb::SystemInfo::get_system_info();
		ASSERT_MSG(systemInfoResult.has_value(), "Failed to get system info, Error: {}", instanceResult.error().message());
		vkb::SystemInfo system_info = systemInfoResult.value();
	}

	void CTXBuilder::_createVulkanSurface(CTX& ctx, SDL_Window* sdlWindow) const {
		// Surface
		VkSurfaceKHR surface = nullptr;
		bool surface_success = SDL_Vulkan_CreateSurface(sdlWindow, ctx._vkInstance, nullptr, &surface);
		ASSERT_MSG(surface_success, "Failed to create Vulkan surface with GLFW");
		ctx._vkSurfaceKHR = surface;
	}

	void CTXBuilder::_createVulkanPhysDevice(CTX& ctx, vkb::Instance& vkb_instance, vkb::PhysicalDevice& vkb_physdevice_EMPTY) const {
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

		vkb::PhysicalDeviceSelector physicalDeviceSelector{vkb_instance};
		vkb::Result<vkb::PhysicalDevice> physicalDeviceResult = physicalDeviceSelector
				.set_minimum_version(1, 2)
				.set_surface(ctx._vkSurfaceKHR)
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

				.set_required_features_13(features13)
				.set_required_features_12(features12)
				.set_required_features_11(features11)
				.set_required_features(features)
				.select();
		ASSERT_MSG(physicalDeviceResult.has_value(), "Failed to select Vulkan Physical Device. Error: {}", physicalDeviceResult.error().message());
		vkb_physdevice_EMPTY = physicalDeviceResult.value();
		ctx._vkPhysicalDevice = physicalDeviceResult.value().physical_device;

		// get physical device properties
		VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		VkPhysicalDeviceVulkan13Properties props13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
		VkPhysicalDeviceVulkan12Properties props12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
		VkPhysicalDeviceVulkan11Properties props11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
		props.pNext = &props13;
		props13.pNext = &props12;
		props12.pNext = &props11;
		vkGetPhysicalDeviceProperties2(ctx._vkPhysicalDevice, &props);
		ctx._vkPhysDeviceProperties = physicalDeviceResult.value().properties;
		ctx._vkPhysDeviceVulkan13Properties = props13;
		ctx._vkPhysDeviceVulkan12Properties = props12;
		ctx._vkPhysDeviceVulkan11Properties = props11;
	}
	void CTXBuilder::_createVulkanLogicalDevice(CTX& ctx, vkb::PhysicalDevice& vkb_physdevice, vkb::Device& vkb_device_EMPTY) const {
		// Logical Device

		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic_state_feature = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
		dynamic_state_feature.pNext = nullptr;
		dynamic_state_feature.extendedDynamicState = VK_TRUE;

		VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamic_state_feature_2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
		dynamic_state_feature_2.pNext = nullptr;
		dynamic_state_feature_2.extendedDynamicState2 = VK_TRUE;

		vkb::DeviceBuilder logicalDeviceBuilder{vkb_physdevice};
		vkb::Result<vkb::Device> logicalDeviceResult = logicalDeviceBuilder
				.add_pNext(&dynamic_state_feature)
				.add_pNext(&dynamic_state_feature_2)
				.build();

		ASSERT_MSG(logicalDeviceResult.has_value(), "Failed to create Vulkan Logical Device. Error: {}", logicalDeviceResult.error().message());
		vkb_device_EMPTY = logicalDeviceResult.value();
		ctx._vkDevice = logicalDeviceResult->device;
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
	}
	void CTXBuilder::_createMemoryAllocator(CTX& ctx) const {
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = ctx._vkPhysicalDevice;
		allocatorInfo.device = ctx._vkDevice;
		allocatorInfo.instance = ctx._vkInstance;
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
		VmaVulkanFunctions vulkanFunctions;
		VkResult volkimport_result = vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vulkanFunctions);
		ASSERT_MSG(volkimport_result == VK_SUCCESS, "Failed to import vulkan functions from Volk for VMA Allocator setup!");
		allocatorInfo.pVulkanFunctions = &vulkanFunctions;
		VkResult allocator_result = vmaCreateAllocator(&allocatorInfo, &ctx._vmaAllocator);
		ASSERT_MSG(allocator_result == VK_SUCCESS, "Failed to create vma Allocator!");
	}
	void CTXBuilder::_createVulkanQueues(CTX& ctx, vkb::Device& vkb_device) const {
		// Queues
		// graphics queue
		vkb::Result<VkQueue_T*> graphics_queue_result = vkb_device.get_queue(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_result.has_value(), "Failed to get graphics queue. Error: {}", graphics_queue_result.error().message());
		ctx._vkGraphicsQueue = graphics_queue_result.value();

		// graphics queue index
		vkb::Result<uint32_t> graphics_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::graphics);
		ASSERT_MSG(graphics_queue_index_result.has_value(), "Failed to get graphics queue index/family. Error: {}", graphics_queue_index_result.error().message());
		ctx._graphicsQueueFamilyIndex = graphics_queue_index_result.value();

		// present queue
		vkb::Result<VkQueue_T*> present_queue_result = vkb_device.get_queue(vkb::QueueType::present);
		ASSERT_MSG(present_queue_result.has_value(), "Failed to get present queue. Error: {}", present_queue_result.error().message());
		ctx._vkPresentQueue = present_queue_result.value();

		// present queue index
		vkb::Result<uint32_t> present_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::present);
		ASSERT_MSG(present_queue_index_result.has_value(), "Failed to get present queue index/family. Error: {}", present_queue_index_result.error().message());
		ctx._presentQueueFamilyIndex = present_queue_index_result.value();
	}

}