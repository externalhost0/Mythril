//
// Created by Hayden Rivas on 1/16/26.
//

#include "mythril/CTX.h"
#include "mythril/Plugins.h"
#include "../HelperMacros.h"
#include "../vkutil.h"
#include "../ImmediateCommands.h"

#include <cstring>

#include <volk.h>
#include <tracy/TracyVulkan.hpp>

#ifdef MYTH_ENABLED_TRACY_GPU
namespace mythril {
	void TracyGPUPlugin::onInit(CTX& ctx) {
		// aliases
		VkInstance vk_instance = ctx._vkInstance;
		VkPhysicalDevice vk_physical_device = ctx._vkPhysicalDevice;
		VkDevice vk_device = ctx._vkDevice;
		this->_ctx = &ctx;

		std::vector<VkTimeDomainEXT> timeDomains;
		const bool hasCalibratedTimestamps = ctx.isExtensionEnabled(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
		if (hasCalibratedTimestamps) {
			uint32_t numTimeDomains = 0;
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(vk_physical_device, &numTimeDomains, nullptr);
			timeDomains.resize(numTimeDomains);
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(vk_physical_device, &numTimeDomains, timeDomains.data());
		}
		const bool hasHostQuery = ctx.getPhysicalDeviceFeatures12().hostQueryReset && [&timeDomains]() -> bool {
			for (const VkTimeDomainEXT domain: timeDomains)
				if (domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT || domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT)
					return true;
			return false;
		}();
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &familyCount, families.data());
		const std::array<ImmediateCommands*, kQueueCount> queues{ctx._immGraphics.get(), ctx._immAsyncCompute.get(), ctx._immAsyncTransfer.get()};
		for (size_t q = 0; q < kQueueCount; ++q) {
			const ImmediateCommands* imm = queues[q];
			if (!imm || families[imm->getQueueFamilyIndex()].timestampValidBits == 0)
				continue;
			if (hasHostQuery) {
				this->_tracyVkCtx[q] = TracyVkContextHostCalibrated(vk_instance, vk_physical_device, vk_device, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
				continue;
			}
			const VkCommandPoolCreateInfo command_pool_ci = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = imm->getQueueFamilyIndex()};
			VK_CHECK(vkCreateCommandPool(vk_device, &command_pool_ci, nullptr, &this->_commandPool[q]));
			vkutil::SetObjectDebugName(vk_device, VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<uint64_t>(this->_commandPool[q]), "TracyGPU: CommandPool");
			const VkCommandBufferAllocateInfo command_buffer_ai = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = this->_commandPool[q], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VK_CHECK(vkAllocateCommandBuffers(vk_device, &command_buffer_ai, &commandBuffer));
			vkutil::SetObjectDebugName(vk_device, VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64_t>(commandBuffer), "TracyGPU: CommandBuffer");
			if (hasCalibratedTimestamps) {
				this->_tracyVkCtx[q] = TracyVkContextCalibrated(vk_instance, vk_physical_device, vk_device, imm->getQueue(), commandBuffer, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
			} else {
				this->_tracyVkCtx[q] = TracyVkContext(vk_instance, vk_physical_device, vk_device, imm->getQueue(), commandBuffer, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
			}
		}
		constexpr std::array<const char*, kQueueCount> kNames{"main", "async compute", "async transfer"};
		for (size_t q = 0; q < kQueueCount; ++q) {
			if (this->_tracyVkCtx[q]) {
				TracyVkContextName(this->_tracyVkCtx[q], kNames[q], strlen(kNames[q]));
			}
		}
		assert(this->_tracyVkCtx[0]);
	}

	void TracyGPUPlugin::onDestroy() {
		for (size_t q = 0; q < kQueueCount; ++q) {
			if (this->_tracyVkCtx[q]) {
				TracyVkDestroy(this->_tracyVkCtx[q]);
			}
			if (this->_ctx && this->_commandPool[q])
				vkDestroyCommandPool(this->_ctx->_vkDevice, this->_commandPool[q], nullptr);
		}
	}
} // namespace mythril
#endif
