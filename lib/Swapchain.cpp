//
// Created by Hayden Rivas on 10/1/25.
//

#include <VkBootstrap.h>

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
		// PRIMARY SWAPCHAIN DATA CREATION //
		uint16_t width = args.width, height = args.height;
		vkb::SwapchainBuilder swapchainBuilder{_ctx._vkPhysicalDevice, _ctx._vkDevice, _ctx._vkSurfaceKHR};
		auto swapchain_result = swapchainBuilder
			.set_desired_format(VkSurfaceFormatKHR{
				.format = args.format,
				.colorSpace = args.colorSpace,
			})
			// .set_desired_min_image_count(vkb::SwapchainBuilder::DOUBLE_BUFFERING)
			.set_desired_present_mode(args.presentMode) // VERY IMPORTANT, decides framerate/buffer/sync
			.set_desired_extent(width, height)
			.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
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
		// things we assign to the textures we will craete
		std::vector<VkImage> images = vkbswapchain.get_images().value();
		std::vector<VkImageView> imageviews = vkbswapchain.get_image_views().value();
		// now for the number of swapchain images available, create a texture handle for it
		VkImageUsageFlags usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		{
			VkSurfaceCapabilitiesKHR caps = {};
			VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR, &caps));

			VkFormatProperties2 format_props2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
			vkGetPhysicalDeviceFormatProperties2(_ctx._vkPhysicalDevice, _vkImageFormat, &format_props2);

			const bool isStorageSupported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) > 0;
			const bool isTilingOptimalSupported = (format_props2.formatProperties.optimalTilingFeatures & VK_IMAGE_USAGE_STORAGE_BIT) > 0;

			if (isStorageSupported && isTilingOptimalSupported) {
				usage_flags |= VK_IMAGE_USAGE_STORAGE_BIT;
			}
		}
		for (uint32_t i = 0; i < _numSwapchainImages; i++) {
			// give necessary info to texture
			AllocatedTexture image = {};
			image._isSwapchainImage = true;
			image._isOwning = false;
			image._vkUsageFlags = usage_flags;
			image._vkImageType = VK_IMAGE_TYPE_2D;
			image._vkImage = images[i],
			image._vkImageView = imageviews[i],
			image._vkCurrentImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			image._vkExtent = { _vkExtent2D.width, _vkExtent2D.height, 1},
			image._vkFormat = _vkImageFormat;
			snprintf(image._debugName, sizeof(image._debugName), "Swapchain Image %d", i);
			char d[64];
			snprintf(d, sizeof(d), "Swapchain Image %d - View", i);
			vkutil::SetObjectDebugName(ctx._vkDevice, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(images[i]), image._debugName);
			vkutil::SetObjectDebugName(ctx._vkDevice, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(imageviews[i]), d);
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
		// images are destroyed alongside swapchain destruction
		vkDestroySwapchainKHR(_ctx._vkDevice, _vkSwapchain, nullptr);
		// DESTROY SECONDARY DATA //
		for (int i = 0; i < _numSwapchainImages; i++) {
			vkDestroySemaphore(_ctx._vkDevice, _vkAcquireSemaphores[i], nullptr);
			if (_vkAcquireFences[i] != VK_NULL_HANDLE) {
				vkDestroyFence(_ctx._vkDevice, _vkAcquireFences[i], nullptr);
			}
		}
	}

	// void Swapchain::acquire() {
	// 	MYTH_PROFILER_FUNCTION();
	// 	if (_getNextImage) {
	// 		const VkSemaphoreWaitInfo waitInfo = {
	// 				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
	// 				.semaphoreCount = 1,
	// 				.pSemaphores = &_ctx._timelineSemaphore,
	// 				.pValues = &_timelineWaitValues[_currentImageIndex],
	// 		};
	// 		ASSERT_MSG(_currentImageIndex < (sizeof(_timelineWaitValues)/sizeof(_timelineWaitValues[0])), "Image index out of range");
	// 		VK_CHECK(vkWaitSemaphores(_ctx._vkDevice, &waitInfo, UINT64_MAX));
	// 		VK_CHECK(vkWaitForFences(_ctx._vkDevice, 1, &_vkAcquireFences[_currentImageIndex], VK_TRUE, UINT64_MAX));
	// 		VK_CHECK(vkResetFences(_ctx._vkDevice, 1, &_vkAcquireFences[_currentImageIndex]));
	//
	// 		// aliases
	// 		VkFence acquireFence = _vkAcquireFences[_currentImageIndex];
	// 		VkSemaphore acquireSemaphore = _vkAcquireSemaphores[_currentImageIndex];
	// 		const VkResult result = vkAcquireNextImageKHR(_ctx._vkDevice, _vkSwapchain, UINT64_MAX, acquireSemaphore, acquireFence, &_currentImageIndex);
	// 		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
	// 			ASSERT(result);
	// 		}
	// 		_getNextImage = false;
	// 		_ctx._imm->waitSemaphore(acquireSemaphore);
	// 	}
	// }
	void Swapchain::acquire() {
		MYTH_PROFILER_FUNCTION();
		if (_getNextImage) {
			// calculate frame in flight index, which is different from image index
			uint32_t frameIndex = _currentFrameNum % _numSwapchainImages;

			// wait for frame N - numSwapchainImages
			const VkSemaphoreWaitInfo waitInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores = &_ctx._timelineSemaphore,
				.pValues = &_timelineWaitValues[frameIndex],
			};
			VK_CHECK(vkWaitSemaphores(_ctx._vkDevice, &waitInfo, UINT64_MAX));

			VK_CHECK(vkWaitForFences(_ctx._vkDevice, 1, &_vkAcquireFences[frameIndex], VK_TRUE, UINT64_MAX));
			VK_CHECK(vkResetFences(_ctx._vkDevice, 1, &_vkAcquireFences[frameIndex]));

			// acquire with this upcoming frames semaphore and fence
			VkSemaphore acquireSemaphore = _vkAcquireSemaphores[frameIndex];
			VkFence acquireFence = _vkAcquireFences[frameIndex];

			const VkResult result = vkAcquireNextImageKHR(
				_ctx._vkDevice,
				_vkSwapchain,
				UINT64_MAX,
				acquireSemaphore,
				acquireFence,
				&_currentImageIndex
			);
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
				ASSERT(result);
			}
			_getNextImage = false;
			_ctx._imm->waitSemaphore(acquireSemaphore);
		}
	}

	void Swapchain::present() {
		MYTH_PROFILER_FUNCTION();
		VkSemaphore semaphore = _ctx._imm->acquireLastSubmitSemaphore();
		const VkPresentInfoKHR present_info = {
				.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.pNext = nullptr,
				.waitSemaphoreCount = 1,
				.pWaitSemaphores = &semaphore,
				.swapchainCount = 1,
				.pSwapchains = &_vkSwapchain,
				.pImageIndices = &_currentImageIndex,
		};
		const VkResult presentResult = vkQueuePresentKHR(_ctx._vkGraphicsQueue, &present_info);
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || _isDirty) {
			_isDirty = true;
			if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) return;
		}
		_getNextImage = true;
		_currentFrameNum++;
	}

}