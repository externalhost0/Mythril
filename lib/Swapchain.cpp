//
// Created by Hayden Rivas on 10/1/25.
//

#include <VkBootstrap.h>

#include <vector>

#include "Swapchain.h"
#include "ImmediateCommands.h"
#include "CTX.h"
#include "HelperMacros.h"
#include "Logger.h"
#include "vkinfo.h"
#include "vkstring.h"
#include "VulkanObjects.h"

namespace mythril {
	Swapchain::Swapchain(CTX& ctx, SwapchainSpec args)
	: _ctx(ctx) {
		ASSERT_MSG(args.width > 0 && args.height > 0, "Swapchain width & height must both be greater than 0!");
		uint16_t width = args.width, height = args.height;

		uint32_t formatCount = 0;
		VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR, &formatCount, nullptr));
		ASSERT_MSG(formatCount > 0, "No surface formats available for swapchain!");
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR, &formatCount, formats.data()));

		VkSurfaceFormatKHR surfaceFormat = formats[0];
		if (args.format != VK_FORMAT_MAX_ENUM) {
			for (const auto& f : formats) {
				if (f.format == args.format && (args.colorSpace == VK_COLOR_SPACE_MAX_ENUM_KHR || f.colorSpace == args.colorSpace)) {
					surfaceFormat = f;
					break;
				}
			}
		} else if (args.colorSpace != VK_COLOR_SPACE_MAX_ENUM_KHR) {
			for (const auto& f : formats) {
				if (f.colorSpace == args.colorSpace) {
					surfaceFormat = f;
					break;
				}
			}
		}

		VkSurfaceCapabilitiesKHR caps = {};
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR, &caps));

		VkFormatProperties2 formatProps2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
		vkGetPhysicalDeviceFormatProperties2(_ctx._vkPhysicalDevice, surfaceFormat.format, &formatProps2);

		const bool storageOk =
				(caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
				(formatProps2.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;

		VkImageUsageFlags extraImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (storageOk) {
			extraImageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}

		vkb::SwapchainBuilder swapchainBuilder{_ctx._vkPhysicalDevice, _ctx._vkDevice, _ctx._vkSurfaceKHR};
		auto swapchain_result = swapchainBuilder
			.set_desired_format(surfaceFormat)
			.set_desired_min_image_count(kNUM_FRAMES_IN_FLIGHT)
			.set_desired_present_mode(args.presentMode) // VERY IMPORTANT, decides framerate/buffer/sync
			.set_desired_extent(width, height)
			.add_image_usage_flags(extraImageUsage)
			.build();
		ASSERT_MSG(swapchain_result.has_value(), "[VULKAN] {}", swapchain_result.error().message());
		vkb::Swapchain& vkbswapchain = swapchain_result.value();
		// things we assign to the class itself
		this->_vkSwapchain = vkbswapchain.swapchain;
		this->_vkExtent2D = vkbswapchain.extent;
		this->_vkColorSpace = vkbswapchain.color_space;
		this->_vkPresentMode = vkbswapchain.present_mode;

		if (_vkExtent2D.width != width || _vkExtent2D.height != height) {
			LOG_SYSTEM(LogType::Warning, "Requested swapchain size did not take place! \n Requested: {} x {} vs Actual: {} x {}", width, height, _vkExtent2D.width, _vkExtent2D.height);
		}
		this->_vkImageFormat = vkbswapchain.image_format;
		if (args.format != _vkImageFormat) {
			LOG_SYSTEM(LogType::Warning, "Requested swapchain format did not take place! \n Requested: '{}' vs Actual: '{}'", vkstring::VulkanFormatToString(args.format), vkstring::VulkanFormatToString(_vkImageFormat));
		}
		if (vkbswapchain.image_count > kMAX_SWAPCHAIN_IMAGES) {
			LOG_SYSTEM(LogType::FatalError, "Swapchain image count ({}) exceeds max supported swapchain images ({})!", (int) vkbswapchain.image_count, (int) kMAX_SWAPCHAIN_IMAGES);
		}
		this->_numSwapchainImages = vkbswapchain.image_count;
		ASSERT(_numSwapchainImages <= kMAX_SWAPCHAIN_IMAGES);
		ASSERT((caps.minImageCount <= _numSwapchainImages) && (_numSwapchainImages <= caps.maxImageCount));

		const VkImageUsageFlags usage_flags =
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				(storageOk ? VK_IMAGE_USAGE_STORAGE_BIT : VkImageUsageFlags{});

		std::vector<VkImage> images = vkbswapchain.get_images().value();
		std::vector<VkImageView> imageviews = vkbswapchain.get_image_views().value();
		for (uint32_t i = 0; i < _numSwapchainImages; i++) {
			// give necessary info to texture
			AllocatedTexture image = {};
			image._isSwapchainImage = true;
			// basically the only time isOwning is false
			image._isOwning = false;
			image._vkUsageFlags = usage_flags;
			image._vkImageType = VK_IMAGE_TYPE_2D;
			image._vkImage = images[i],
			image._vkImageView = imageviews[i],
			image._vkCurrentImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			image._vkExtent = { _vkExtent2D.width, _vkExtent2D.height, 1},
			image._vkFormat = _vkImageFormat;
			snprintf(image._debugName, sizeof(image._debugName), "Swapchain Image %d", i);
			vkutil::SetObjectDebugName(ctx._vkDevice, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(images[i]), image._debugName);
			char temp_buf[64];
			snprintf(temp_buf, sizeof(temp_buf), "Swapchain Image View %d", i);
			vkutil::SetObjectDebugName(ctx._vkDevice, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(imageviews[i]), temp_buf);
			this->_swapchainTextures[i] = _ctx._texturePool.create(std::move(image));
		}
		// SECONDARY DATA CREATION //
		VkSemaphoreCreateInfo semaphoreInfo = vkinfo::CreateSemaphoreInfo();
		VkFenceCreateInfo fenceInfo = vkinfo::CreateFenceInfo(VK_FENCE_CREATE_SIGNALED_BIT);
		for (int i = 0; i < _numSwapchainImages; i++) {
			VK_CHECK(vkCreateSemaphore(_ctx._vkDevice, &semaphoreInfo, nullptr, &_vkAcquireSemaphores[i]));
			VK_CHECK(vkCreateFence(_ctx._vkDevice, &fenceInfo, nullptr, &_vkAcquireFences[i]));
		}
	}

	Swapchain::~Swapchain() {
		// DESTROY MAIN SWAPCHAIN DATA //
		for (int i = 0; i < _numSwapchainImages; i++) {
			_ctx.destroy(_swapchainTextures[i]);
		}
		vkDestroySwapchainKHR(_ctx._vkDevice, _vkSwapchain, nullptr);
		// DESTROY SECONDARY DATA //
		for (int i = 0; i < _numSwapchainImages; i++) {
			vkDestroySemaphore(_ctx._vkDevice, _vkAcquireSemaphores[i], nullptr);
			if (_vkAcquireFences[i] != VK_NULL_HANDLE) {
				vkDestroyFence(_ctx._vkDevice, _vkAcquireFences[i], nullptr);
			}
		}
	}

	VkSemaphore Swapchain::acquire() {
		MYTH_PROFILER_FUNCTION_N("Swapchain Acquire");
		ASSERT_MSG(_getNextImage, "Already acquired this frame's acquire semaphore!");

		// wait for frame N - numSwapchainImages
		const VkSemaphoreWaitInfo waitInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &_ctx._timelineSemaphore,
			.pValues = &_timelineWaitValues[_frameIndex],
		};
		VK_CHECK(vkWaitSemaphores(_ctx._vkDevice, &waitInfo, UINT64_MAX));

		VkSemaphore vkAcquireSemaphore = _vkAcquireSemaphores[_frameIndex];
		const VkResult result = vkAcquireNextImageKHR(
			_ctx._vkDevice,
			_vkSwapchain,
			UINT64_MAX,
			vkAcquireSemaphore,
			VK_NULL_HANDLE,
			&_imageIndex
		);
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
			ASSERT_MSG(result, "Something went horribly wrong, vkAcquireNextImageKHR() failed!");
		}
		_getNextImage = false;
		return vkAcquireSemaphore;
	}

	void Swapchain::present(VkSemaphore waitSemaphore) {
		MYTH_PROFILER_FUNCTION_N("Swapchain Present");
		const VkPresentInfoKHR present_info = {
				.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = 1,
				.pWaitSemaphores = &waitSemaphore,
				.swapchainCount = 1,
				.pSwapchains = &_vkSwapchain,
				.pImageIndices = &_imageIndex,
		};
		const VkResult presentResult = vkQueuePresentKHR(_ctx._vkGraphicsQueue, &present_info);
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || _isDirty) {
			_isDirty = true;
			if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) return;
		}
		_getNextImage = true;
		_frameIndex = (_frameIndex + 1) % kNUM_FRAMES_IN_FLIGHT;
		MYTH_PROFILER_FRAME();
	}
}
