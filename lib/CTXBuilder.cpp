//
// Created by Hayden Rivas on 10/5/25.
//


#include "mythril/CTXBuilder.h"

#include <set>

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
		Version appVersion;
		Version engineVersion;
		const char* appName = nullptr;
		const char* engineName = nullptr;
	};
	static vkb::Instance CreateVulkanInstance(VulkanInstanceInputs inputs, bool enableValidation, bool enableHeadless, std::span<const char*> instance_extensions) {
		vkb::InstanceBuilder instanceBuilder;
		{
			auto systemInfoResult = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
			ASSERT_MSG(systemInfoResult, "System Information could not be retrieved, Vulkan is not correctly installed on your system. Error: {}", systemInfoResult.error().message());
			const auto& systemInfo = systemInfoResult.value();
			std::string missing_extensions;
			for (const auto& ext: instance_extensions) {
				if (!systemInfo.is_extension_available(ext)) {
					missing_extensions += "\n\t" + std::string(ext);
				} else {
					instanceBuilder.enable_extension(ext);
				}
			}
			if (!missing_extensions.empty()) {
				LOG_SYSTEM_NOSOURCE(LogType::FatalError, "System fails to support Vulkan Instance Extensions: {}", missing_extensions.c_str());
				assert(false);
			}
		}
		vkb::Result<vkb::Instance> instanceResult = instanceBuilder
				.set_app_name(inputs.appName)
				.set_app_version(inputs.appVersion.getVKVersion())
				.set_engine_name(inputs.engineName)
				.set_engine_version(inputs.engineVersion.getVKVersion())
				.require_api_version(VK_API_VERSION_1_3)
				.set_minimum_instance_version(1, 4, 304)
				.request_validation_layers(enableValidation)
				.set_headless(enableHeadless)
#ifdef DEBUG
		// we dont need to worry about conditionally having this be called or not depending on enableValidation, its handled internally
				.use_default_debug_messenger()
#endif
				.build();
		ASSERT_MSG(instanceResult.has_value(), "Failed to create Instance (VkInstance). Error: {}", instanceResult.error().message());
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
		ASSERT_MSG(physicalDeviceResult.has_value(), "Failed to select Physical Device (VkPhysicalDevice). Error: {}", physicalDeviceResult.error().message());
		return physicalDeviceResult.value();
	}
	struct VulkanQueueOutputs
	{
		VkQueue vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t graphicsQueueFamilyIndex = Invalid<uint32_t>;
		VkQueue vkPresentQueue = VK_NULL_HANDLE;
		uint32_t presentQueueFamilyIndex = Invalid<uint32_t>;
		VkQueue vkComputeQueue = VK_NULL_HANDLE;
		uint32_t computeQueueFamilyIndex = Invalid<uint32_t>;
		VkQueue vkTransferQueue = VK_NULL_HANDLE;
		uint32_t transferQueueFamilyIndex = Invalid<uint32_t>;
	};

	static void GetFamilyQueueIndices() {

	}


	static VkDevice CreateVulkanLogicalDevice(
		vkb::PhysicalDevice& vkbPhysicalDevice,
		std::span<const char *> user_device_extensions,
		void* user_device_extension_features,
		VulkanFeatures& outFeatures,
		VulkanProperties& outProperties,
		VulkanQueueOutputs& outQueues) {
		ASSERT(vkbPhysicalDevice.physical_device != VK_NULL_HANDLE);

		// VALIDATE EXTENSIONS
		{
			std::string missing_extensions;
			for (const auto& ext : user_device_extensions) {
				if (!vkbPhysicalDevice.enable_extension_if_present(ext)) {
					missing_extensions += "\n\t" + std::string(ext);
				}
			}
			if (!missing_extensions.empty()) {
				LOG_SYSTEM_NOSOURCE(LogType::FatalError, "Device fails to support Vulkan Device Extensions: {}", missing_extensions.c_str());
				assert(false);
			}
		}
		std::vector<const char*> enabledExtensions = {};
		// get extensions
		std::vector<std::string> requestedExtensions = vkbPhysicalDevice.get_extensions();
		enabledExtensions.reserve(requestedExtensions.size());
		for (const auto& ext: requestedExtensions) {
			enabledExtensions.push_back(ext.c_str());
		}
		// add swapchain extension if necessary (usually necessary
		if (vkbPhysicalDevice.surface != VK_NULL_HANDLE) {
			enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		}


		// query supported features
		VkPhysicalDeviceFeatures2 supportedfeatures10 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceVulkan11Features supportedfeatures11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		VkPhysicalDeviceVulkan12Features supportedfeatures12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceVulkan13Features supportedfeatures13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		supportedfeatures10.pNext = &supportedfeatures11;
		supportedfeatures11.pNext = &supportedfeatures12;
		supportedfeatures12.pNext = &supportedfeatures13;
		vkGetPhysicalDeviceFeatures2(vkbPhysicalDevice.physical_device, &supportedfeatures10);


		// the following features that are VK_TRUE are required by Mythril
		// others can be optionally used when supported
		constexpr VkPhysicalDeviceFeatures requiredfeatures10 = {
			.robustBufferAccess = VK_FALSE,
			.multiDrawIndirect = VK_TRUE,
			.depthBiasClamp = VK_TRUE,
			.samplerAnisotropy = VK_TRUE,
			.fragmentStoresAndAtomics = VK_TRUE,
		};
		VkPhysicalDeviceVulkan11Features requiredfeatures11 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
			.pNext = user_device_extension_features != nullptr ? user_device_extension_features : nullptr,
			.multiview = VK_TRUE,
			.shaderDrawParameters = VK_TRUE
		};
		VkPhysicalDeviceVulkan12Features requiredfeatures12 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &requiredfeatures11,
			.descriptorIndexing = VK_TRUE,
			.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
			.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
			.descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
			.descriptorBindingPartiallyBound = VK_TRUE,
			.runtimeDescriptorArray = VK_TRUE,
			.scalarBlockLayout = VK_TRUE, // opt
			.timelineSemaphore = VK_TRUE,
			.bufferDeviceAddress = VK_TRUE,
		};
		VkPhysicalDeviceVulkan13Features requiredfeatures13 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &requiredfeatures12,
			.computeFullSubgroups = VK_TRUE,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};

		// VALIDATE FEATURES
		{
			std::string missing_features;
#define CHECK_VULKAN_FEATURE(requiredFeatures, availableFeatures, feature, version) \
if ((requiredFeatures.feature) == VK_TRUE && (availableFeatures.feature) == VK_FALSE) \
missing_features.append("\n\t(" version ") " #feature);
#define CHECK_FEATURE_1_0(feature) CHECK_VULKAN_FEATURE(requiredfeatures10, supportedfeatures10.features, feature, "1.0")
			CHECK_FEATURE_1_0(robustBufferAccess);
			CHECK_FEATURE_1_0(fullDrawIndexUint32);
			CHECK_FEATURE_1_0(imageCubeArray);
			CHECK_FEATURE_1_0(independentBlend);
			CHECK_FEATURE_1_0(geometryShader);
			CHECK_FEATURE_1_0(tessellationShader);
			CHECK_FEATURE_1_0(sampleRateShading);
			CHECK_FEATURE_1_0(dualSrcBlend);
			CHECK_FEATURE_1_0(logicOp);
			CHECK_FEATURE_1_0(multiDrawIndirect);
			CHECK_FEATURE_1_0(drawIndirectFirstInstance);
			CHECK_FEATURE_1_0(depthClamp);
			CHECK_FEATURE_1_0(depthBiasClamp);
			CHECK_FEATURE_1_0(fillModeNonSolid);
			CHECK_FEATURE_1_0(depthBounds);
			CHECK_FEATURE_1_0(wideLines);
			CHECK_FEATURE_1_0(largePoints);
			CHECK_FEATURE_1_0(alphaToOne);
			CHECK_FEATURE_1_0(multiViewport);
			CHECK_FEATURE_1_0(samplerAnisotropy);
			CHECK_FEATURE_1_0(textureCompressionETC2);
			CHECK_FEATURE_1_0(textureCompressionASTC_LDR);
			CHECK_FEATURE_1_0(textureCompressionBC);
			CHECK_FEATURE_1_0(occlusionQueryPrecise);
			CHECK_FEATURE_1_0(pipelineStatisticsQuery);
			CHECK_FEATURE_1_0(vertexPipelineStoresAndAtomics);
			CHECK_FEATURE_1_0(fragmentStoresAndAtomics);
			CHECK_FEATURE_1_0(shaderTessellationAndGeometryPointSize);
			CHECK_FEATURE_1_0(shaderImageGatherExtended);
			CHECK_FEATURE_1_0(shaderStorageImageExtendedFormats);
			CHECK_FEATURE_1_0(shaderStorageImageMultisample);
			CHECK_FEATURE_1_0(shaderStorageImageReadWithoutFormat);
			CHECK_FEATURE_1_0(shaderStorageImageWriteWithoutFormat);
			CHECK_FEATURE_1_0(shaderUniformBufferArrayDynamicIndexing);
			CHECK_FEATURE_1_0(shaderSampledImageArrayDynamicIndexing);
			CHECK_FEATURE_1_0(shaderStorageBufferArrayDynamicIndexing);
			CHECK_FEATURE_1_0(shaderStorageImageArrayDynamicIndexing);
			CHECK_FEATURE_1_0(shaderClipDistance);
			CHECK_FEATURE_1_0(shaderCullDistance);
			CHECK_FEATURE_1_0(shaderFloat64);
			CHECK_FEATURE_1_0(shaderInt64);
			CHECK_FEATURE_1_0(shaderInt16);
			CHECK_FEATURE_1_0(shaderResourceResidency);
			CHECK_FEATURE_1_0(shaderResourceMinLod);
			CHECK_FEATURE_1_0(sparseBinding);
			CHECK_FEATURE_1_0(sparseResidencyBuffer);
			CHECK_FEATURE_1_0(sparseResidencyImage2D);
			CHECK_FEATURE_1_0(sparseResidencyImage3D);
			CHECK_FEATURE_1_0(sparseResidency2Samples);
			CHECK_FEATURE_1_0(sparseResidency4Samples);
			CHECK_FEATURE_1_0(sparseResidency8Samples);
			CHECK_FEATURE_1_0(sparseResidency16Samples);
			CHECK_FEATURE_1_0(sparseResidencyAliased);
			CHECK_FEATURE_1_0(variableMultisampleRate);
			CHECK_FEATURE_1_0(inheritedQueries);
#undef CHECK_FEATURE_1_0
#define CHECK_FEATURE_1_1(feature) CHECK_VULKAN_FEATURE(requiredfeatures11, supportedfeatures11, feature, "1.1")
			CHECK_FEATURE_1_1(storageBuffer16BitAccess);
			CHECK_FEATURE_1_1(uniformAndStorageBuffer16BitAccess);
			CHECK_FEATURE_1_1(storagePushConstant16);
			CHECK_FEATURE_1_1(storageInputOutput16);
			CHECK_FEATURE_1_1(multiview);
			CHECK_FEATURE_1_1(multiviewGeometryShader);
			CHECK_FEATURE_1_1(multiviewTessellationShader);
			CHECK_FEATURE_1_1(variablePointersStorageBuffer);
			CHECK_FEATURE_1_1(variablePointers);
			CHECK_FEATURE_1_1(protectedMemory);
			CHECK_FEATURE_1_1(samplerYcbcrConversion);
			CHECK_FEATURE_1_1(shaderDrawParameters);
#undef CHECK_FEATURE_1_1
#define CHECK_FEATURE_1_2(feature) CHECK_VULKAN_FEATURE(requiredfeatures12, supportedfeatures12, feature, "1.2")
			CHECK_FEATURE_1_2(samplerMirrorClampToEdge);
			CHECK_FEATURE_1_2(drawIndirectCount);
			CHECK_FEATURE_1_2(storageBuffer8BitAccess);
			CHECK_FEATURE_1_2(uniformAndStorageBuffer8BitAccess);
			CHECK_FEATURE_1_2(storagePushConstant8);
			CHECK_FEATURE_1_2(shaderBufferInt64Atomics);
			CHECK_FEATURE_1_2(shaderSharedInt64Atomics);
			CHECK_FEATURE_1_2(shaderFloat16);
			CHECK_FEATURE_1_2(shaderInt8);
			CHECK_FEATURE_1_2(descriptorIndexing);
			CHECK_FEATURE_1_2(shaderInputAttachmentArrayDynamicIndexing);
			CHECK_FEATURE_1_2(shaderUniformTexelBufferArrayDynamicIndexing);
			CHECK_FEATURE_1_2(shaderStorageTexelBufferArrayDynamicIndexing);
			CHECK_FEATURE_1_2(shaderUniformBufferArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderSampledImageArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderStorageBufferArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderStorageImageArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderInputAttachmentArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderUniformTexelBufferArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(shaderStorageTexelBufferArrayNonUniformIndexing);
			CHECK_FEATURE_1_2(descriptorBindingUniformBufferUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingSampledImageUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingStorageImageUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingStorageBufferUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingUniformTexelBufferUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingStorageTexelBufferUpdateAfterBind);
			CHECK_FEATURE_1_2(descriptorBindingUpdateUnusedWhilePending);
			CHECK_FEATURE_1_2(descriptorBindingPartiallyBound);
			CHECK_FEATURE_1_2(descriptorBindingVariableDescriptorCount);
			CHECK_FEATURE_1_2(runtimeDescriptorArray);
			CHECK_FEATURE_1_2(samplerFilterMinmax);
			CHECK_FEATURE_1_2(scalarBlockLayout);
			CHECK_FEATURE_1_2(imagelessFramebuffer);
			CHECK_FEATURE_1_2(uniformBufferStandardLayout);
			CHECK_FEATURE_1_2(shaderSubgroupExtendedTypes);
			CHECK_FEATURE_1_2(separateDepthStencilLayouts);
			CHECK_FEATURE_1_2(hostQueryReset);
			CHECK_FEATURE_1_2(timelineSemaphore);
			CHECK_FEATURE_1_2(bufferDeviceAddress);
			CHECK_FEATURE_1_2(bufferDeviceAddressCaptureReplay);
			CHECK_FEATURE_1_2(bufferDeviceAddressMultiDevice);
			CHECK_FEATURE_1_2(vulkanMemoryModel);
			CHECK_FEATURE_1_2(vulkanMemoryModelDeviceScope);
			CHECK_FEATURE_1_2(vulkanMemoryModelAvailabilityVisibilityChains);
			CHECK_FEATURE_1_2(shaderOutputViewportIndex);
			CHECK_FEATURE_1_2(shaderOutputLayer);
			CHECK_FEATURE_1_2(subgroupBroadcastDynamicId);
#undef CHECK_FEATURE_1_2
#define CHECK_FEATURE_1_3(feature) CHECK_VULKAN_FEATURE(requiredfeatures13, supportedfeatures13, feature, "1.3")
			CHECK_FEATURE_1_3(robustImageAccess);
			CHECK_FEATURE_1_3(inlineUniformBlock);
			CHECK_FEATURE_1_3(descriptorBindingInlineUniformBlockUpdateAfterBind);
			CHECK_FEATURE_1_3(pipelineCreationCacheControl);
			CHECK_FEATURE_1_3(privateData);
			CHECK_FEATURE_1_3(shaderDemoteToHelperInvocation);
			CHECK_FEATURE_1_3(shaderTerminateInvocation);
			CHECK_FEATURE_1_3(subgroupSizeControl);
			CHECK_FEATURE_1_3(computeFullSubgroups);
			CHECK_FEATURE_1_3(synchronization2);
			CHECK_FEATURE_1_3(textureCompressionASTC_HDR);
			CHECK_FEATURE_1_3(shaderZeroInitializeWorkgroupMemory);
			CHECK_FEATURE_1_3(dynamicRendering);
			CHECK_FEATURE_1_3(shaderIntegerDotProduct);
			CHECK_FEATURE_1_3(maintenance4);
#undef CHECK_FEATURE_1_3
#undef CHECK_VULKAN_FEATURE
			if (!missing_features.empty()) {
				LOG_SYSTEM_NOSOURCE(LogType::FatalError, "Device fails to support Vulkan Device Features: {}", missing_features.c_str());
				assert(false);
			}
		}

#ifdef VK_ENABLE_BETA_EXTENSIONS
		VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_subset_features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR,
			.pNext = &requiredfeatures13,
			.imageViewFormatSwizzle = VK_TRUE
		};
#endif

		VkPhysicalDeviceFeatures2 features2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
#ifdef VK_ENABLE_BETA_EXTENSIONS
			.pNext = &portability_subset_features,
#else
			.pNext = requiredfeatures13
#endif
			.features = requiredfeatures10
		};

		// query queue family properties
		VkPhysicalDevice physical_device = vkbPhysicalDevice.physical_device;
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties2(physical_device, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties2> props(queueFamilyCount);
		for (auto& p : props) p.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		vkGetPhysicalDeviceQueueFamilyProperties2(physical_device, &queueFamilyCount, props.data());

		// for graphics, transfer, compute
		auto findBestFamilyIndex = [&props](VkQueueFlags requiredFlags, VkQueueFlags avoidedFlags) -> uint32_t {
			uint32_t fallback = Invalid<uint32_t>;
			for (uint32_t i = 0; i < static_cast<uint32_t>(props.size()); i++) {
				const VkQueueFamilyProperties& p = props[i].queueFamilyProperties;
				if (!p.queueCount) continue;
				if ((p.queueFlags & requiredFlags) != requiredFlags) continue;
				if ((p.queueFlags & avoidedFlags) == 0)
					return i;
				if (fallback == Invalid<uint32_t>)
					fallback = i;
			}
			return fallback; // might still be Invalid<>
		};
		// for present
		auto findBestPresentFamilyIndex = [&](VkSurfaceKHR surface) -> uint32_t {
			uint32_t fallback           = Invalid<uint32_t>;
			uint32_t fallbackWithGraphics = Invalid<uint32_t>;

			for (uint32_t i = 0; i < static_cast<uint32_t>(props.size()); i++) {
				const VkQueueFamilyProperties& p = props[i].queueFamilyProperties;
				if (!p.queueCount) continue;

				VkBool32 supported = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &supported);
				if (!supported) continue;

				bool hasGraphics = (p.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

				if (p.queueFlags == 0)
					return i;
				if (hasGraphics && fallbackWithGraphics == Invalid<uint32_t>)
					fallbackWithGraphics = i;
				else if (fallback == Invalid<uint32_t>)
					fallback = i;
			}
			// prefer presentation family that supports graphics rather than just graphics queue family
			return fallbackWithGraphics != Invalid<uint32_t> ? fallbackWithGraphics : fallback;
		};

		uint32_t graphics = findBestFamilyIndex(VK_QUEUE_GRAPHICS_BIT, 0);
		uint32_t compute  = findBestFamilyIndex(VK_QUEUE_COMPUTE_BIT,  VK_QUEUE_GRAPHICS_BIT);
		uint32_t transfer = findBestFamilyIndex(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
		uint32_t present  = findBestPresentFamilyIndex(vkbPhysicalDevice.surface);
		ASSERT_MSG(graphics != Invalid<uint32_t>, "No graphics queue family found");
		ASSERT_MSG(present  != Invalid<uint32_t>, "No present queue family found");
		// if no dedicated compute/transfer family exists, share with graphics
		if (compute  == Invalid<uint32_t>) compute  = graphics;
		if (transfer == Invalid<uint32_t>) transfer = graphics;

		struct QueueAllocation {
			uint32_t familyIndex;
			uint32_t queueIndex; // index within that family
		};
		std::unordered_map<uint32_t, uint32_t> familyNextIndex;
		auto allocateQueue = [&](uint32_t familyIndex) -> QueueAllocation {
			uint32_t idx = familyNextIndex[familyIndex]++;
			uint32_t maxQueues = props[familyIndex].queueFamilyProperties.queueCount;
			if (idx >= maxQueues) {
				idx = maxQueues - 1;
				familyNextIndex[familyIndex] = maxQueues;
			}
			return { familyIndex, idx };
		};
		QueueAllocation graphicsAlloc  = allocateQueue(graphics);
		QueueAllocation computeAlloc   = allocateQueue(compute);
		QueueAllocation transferAlloc  = allocateQueue(transfer);
		QueueAllocation presentAlloc   = allocateQueue(present);

		std::vector<std::vector<float>> queuePriorities;
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

		for (auto& [familyIndex, allocatedCount] : familyNextIndex) {
			queuePriorities.emplace_back(allocatedCount, 1.0f);

			queueCreateInfos.push_back({
				.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = familyIndex,
				.queueCount       = allocatedCount,
				.pQueuePriorities = queuePriorities.back().data(),
			});
		}

		VkDeviceCreateInfo device_ci = {
			.sType                  = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext                  = &features2,
			.flags                  = 0,
			.queueCreateInfoCount   = static_cast<uint32_t>(queueCreateInfos.size()),
			.pQueueCreateInfos      = queueCreateInfos.data(),
			.enabledExtensionCount  = static_cast<uint32_t>(enabledExtensions.size()),
			.ppEnabledExtensionNames = enabledExtensions.data(),
		};

		VkDevice device = VK_NULL_HANDLE;
		vkCreateDevice(vkbPhysicalDevice.physical_device, &device_ci, nullptr, &device);
		ASSERT_MSG(device != VK_NULL_HANDLE, "Failed to create Device (VkDevice)");
		volkLoadDevice(device);

		// retrieve actual VkQueue objects
		vkGetDeviceQueue(device, graphicsAlloc.familyIndex,  graphicsAlloc.queueIndex,  &outQueues.vkGraphicsQueue);
		outQueues.graphicsQueueFamilyIndex = graphicsAlloc.familyIndex;

		vkGetDeviceQueue(device, computeAlloc.familyIndex,   computeAlloc.queueIndex,   &outQueues.vkComputeQueue);
		outQueues.computeQueueFamilyIndex  = computeAlloc.familyIndex;

		vkGetDeviceQueue(device, transferAlloc.familyIndex,  transferAlloc.queueIndex,  &outQueues.vkTransferQueue);
		outQueues.transferQueueFamilyIndex = transferAlloc.familyIndex;

		vkGetDeviceQueue(device, presentAlloc.familyIndex,   presentAlloc.queueIndex,   &outQueues.vkPresentQueue);
		outQueues.presentQueueFamilyIndex  = presentAlloc.familyIndex;

		// we could do this before building the device but theres no difference
		// query supported properties
		{
			VkPhysicalDeviceProperties2 props10 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			VkPhysicalDeviceVulkan11Properties props11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
			VkPhysicalDeviceVulkan12Properties props12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
			VkPhysicalDeviceVulkan13Properties props13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
			props10.pNext = &props11;
			props11.pNext = &props12;
			props12.pNext = &props13;
			vkGetPhysicalDeviceProperties2(vkbPhysicalDevice.physical_device, &props10);
			// set outProperties
			outProperties.props10 = props10.properties;
			outProperties.props11 = props11;
			outProperties.props12 = props12;
			outProperties.props13 = props13;

			// set outFeatures
			outFeatures.features10 = requiredfeatures10;
			outFeatures.features11 = requiredfeatures11;
			outFeatures.features12 = requiredfeatures12;
			outFeatures.features13 = requiredfeatures13;
		}
		return device;
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
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "Could not retrieve a present queue.");
		}

		// compute queue (optional)
		if (auto queue = vkb_device.get_queue(vkb::QueueType::compute); queue.has_value()) {
			output.vkComputeQueue = queue.value();
			if (auto index = vkb_device.get_queue_index(vkb::QueueType::compute); index.has_value()) {
				output.computeQueueFamilyIndex = index.value();
			}
		} else {
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "Could not retrieve a compute queue.");
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
		std::vector instance_extensions(std::begin(this->_vulkanCfg.instanceExtensions), std::end(this->_vulkanCfg.instanceExtensions));
		vkb::Instance vkb_instance = CreateVulkanInstance({
			.appVersion = this->_vulkanCfg.app_version,
			.engineVersion = this->_vulkanCfg.engine_version,
			.appName = this->_vulkanCfg.app_name,
			.engineName = this->_vulkanCfg.engine_name,
		}, this->_vulkanCfg.enableValidation, false, instance_extensions);

		VkSurfaceKHR vk_surface = CreateVulkanSurface({
			.vkInstance = vkb_instance.instance,
			.sdlWindow = window._sdlWindow
		});

		// do not pass features or extensions here, do that to the vkb::PhysicalDevice
		vkb::PhysicalDevice vkb_physical_device = SelectVulkanPhysicalDevice({
			.vkbInstance = vkb_instance,
			.vkSurface = vk_surface
		});

		// fill in user device extensions & resolve based on cfg
		std::vector device_extensions(std::begin(this->_vulkanCfg.deviceExtensions), std::end(this->_vulkanCfg.deviceExtensions));
		VulkanFeatures features{};
		VulkanProperties properties{};
		VulkanQueueOutputs queues{};
		// sets features & properties
		VkDevice vk_device = CreateVulkanLogicalDevice(
			vkb_physical_device,
			device_extensions,
			this->_vulkanCfg.deviceExtensionFeatureChain,
			features,
			properties,
			queues);
		// auto q = CreateVulkanQueues(vkb_device);
		VmaAllocator vma_allocator = CreateVulkanMemoryAllocator({
			.vkInstance = vkb_instance.instance,
			.vkPhysicalDevice = vkb_physical_device.physical_device,
			.vkDevice = vk_device
		});

		// most of the setup we need to worry about is done in the constructor
		// this is the same thing as make_unique, ignore the warning
		auto ctx = std::unique_ptr<CTX>(new CTX());
		// manually transfer the values recieved previously to ctx

		// important handles
		ctx->_vkInstance = vkb_instance.instance;
		ctx->_vkDebugMessenger = vkb_instance.debug_messenger;
		ctx->_vkSurfaceKHR = vk_surface;
		ctx->_vkPhysicalDevice = vkb_physical_device.physical_device;
		ctx->_vkDevice = vk_device;
		ctx->_vmaAllocator = vma_allocator;
		// info
		ctx->_featuresVulkan = features;
		ctx->_propertiesVulkan = properties;

		// insert extensions that were properly enabeld
		ctx->_enabledExtensionNames.clear();
		for (const auto& ext : vkb_physical_device.get_extensions()) {
			ctx->_enabledExtensionNames.insert(ext);
		}

		// queues
		ctx->_vkGraphicsQueue = queues.vkGraphicsQueue;
		ctx->_graphicsQueueFamilyIndex = queues.graphicsQueueFamilyIndex;
		ctx->_vkPresentQueue = queues.vkPresentQueue;
		ctx->_presentQueueFamilyIndex = queues.presentQueueFamilyIndex;
		ctx->_vkComputeQueue = queues.vkComputeQueue;
		ctx->_computeQueueFamilyIndex = queues.computeQueueFamilyIndex;

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
			if (this->_swapchainSpec.format == VK_FORMAT_UNDEFINED) {
				this->_swapchainSpec.format = VK_FORMAT_B8G8R8A8_UNORM;
			}
			if (this->_swapchainSpec.colorSpace == VK_COLOR_SPACE_MAX_ENUM_KHR) {
				this->_swapchainSpec.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			}
			if (this->_swapchainSpec.presentMode == VK_PRESENT_MODE_MAX_ENUM_KHR) {
				this->_swapchainSpec.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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