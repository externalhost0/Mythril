//
// Created by Hayden Rivas on 1/16/26.
//

#include "../CTX.h"
#include "../Plugins.h"

#include <volk.h>


namespace mythril {
	void TracyPlugin::onInit(CTX& ctx) {
		// aliases
		VkInstance vk_instance = ctx._vkInstance;
		VkPhysicalDevice vk_physical_device = ctx._vkPhysicalDevice;
		VkDevice vk_device = ctx._vkDevice;
		this->_ctx = &ctx;
		this->_isEnabled = true;

		std::vector<VkTimeDomainEXT> timeDomains;
		const bool hasCalibratedTimestamps = ctx.isExtensionEnabled(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
		if (hasCalibratedTimestamps) {
			uint32_t numTimeDomains = 0;
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(vk_physical_device, &numTimeDomains, nullptr);
			timeDomains.resize(numTimeDomains);
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(vk_physical_device, &numTimeDomains, timeDomains.data());
		}
		const bool hasHostQuery = ctx.getPhysicalDeviceFeatures12().hostQueryReset && [&timeDomains]() -> bool {
			for (const VkTimeDomainEXT domain : timeDomains)
				if (domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT || domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT)
					return true;
			return false;
		}();
		if (hasHostQuery) {
			this->_tracyVkCtx = TracyVkContextHostCalibrated(vk_instance, vk_physical_device, vk_device, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
		} else {
			const VkCommandPoolCreateInfo command_pool_ci = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = ctx._graphicsQueueFamilyIndex
			};
			VK_CHECK(vkCreateCommandPool(vk_device, &command_pool_ci, nullptr, &this->_commandPool));
			vkutil::SetObjectDebugName(vk_device, VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<uint64_t>(this->_commandPool), "TracyGPU: CommandPool");
			const VkCommandBufferAllocateInfo command_buffer_ai = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = this->_commandPool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1
			};
			VK_CHECK(vkAllocateCommandBuffers(vk_device, &command_buffer_ai, &this->_commandBuffer));
			vkutil::SetObjectDebugName(vk_device, VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64_t>(this->_commandBuffer), "TracyGPU: CommandBuffer");
			if (hasCalibratedTimestamps) {
				this->_tracyVkCtx = TracyVkContextCalibrated(vk_instance, vk_physical_device, vk_device, ctx._vkGraphicsQueue, this->_commandBuffer, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
			} else {
				this->_tracyVkCtx = TracyVkContext(vk_instance, vk_physical_device, vk_device, ctx._vkGraphicsQueue, this->_commandBuffer, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
			}
		}
		assert(this->_tracyVkCtx);
	}

	void TracyPlugin::onDestroy() {
		TracyVkDestroy(this->_tracyVkCtx);
		if (this->_ctx && this->_commandPool) {
			vkDestroyCommandPool(this->_ctx->_vkDevice, this->_commandPool, nullptr);
		}
	}
}
