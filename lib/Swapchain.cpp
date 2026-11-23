//
// Created by Hayden Rivas on 10/1/25.
//

#include <vector>
#include <algorithm>

#include "mythril/CTX.h"
#include "HelperMacros.h"
#include "ImmediateCommands.h"
#include "Logger.h"
#include "Swapchain.h"
#include "VulkanObjects.h"
#include "vkinfo.h"
#include <vulkan/vk_enum_string_helper.h>

#include "mythril/CPUProfiler.h"

namespace mythril {
	using namespace detail;
	[[maybe_unused]] static const char* PresentModeName(VkPresentModeKHR mode) {
		switch (mode) {
			case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
			case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
			case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
			case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
			default: return "OTHER";
		}
	}
	[[maybe_unused]] static const char* ColorSpaceName(VkColorSpaceKHR space) {
		switch (space) {
			case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR_KHR";
			case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "DISPLAY_P3_NONLINEAR_EXT";
			case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "EXTENDED_SRGB_LINEAR_EXT";
			case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT: return "DISPLAY_P3_LINEAR_EXT";
			case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT: return "DCI_P3_NONLINEAR_EXT";
			case VK_COLOR_SPACE_BT709_LINEAR_EXT: return "BT709_LINEAR_EXT";
			case VK_COLOR_SPACE_BT709_NONLINEAR_EXT: return "BT709_NONLINEAR_EXT";
			case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return "BT2020_LINEAR_EXT";
			case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084_EXT";
			case VK_COLOR_SPACE_DOLBYVISION_EXT: return "DOLBYVISION_EXT";
			case VK_COLOR_SPACE_HDR10_HLG_EXT: return "HDR10_HLG_EXT";
			case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT: return "ADOBERGB_LINEAR_EXT";
			case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT: return "ADOBERGB_NONLINEAR_EXT";
			case VK_COLOR_SPACE_PASS_THROUGH_EXT: return "PASS_THROUGH_EXT";
			case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return "EXTENDED_SRGB_NONLINEAR_EXT";
			case VK_COLOR_SPACE_DISPLAY_NATIVE_AMD: return "DISPLAY_NATIVE_AMD";
			case VK_COLOR_SPACE_MAX_ENUM_KHR: return "MAX_ENUM_KHR";
			default: return "UNKNOWN";
		}
	}

	Swapchain::Swapchain(CTX& ctx, SwapchainSpec args) :
	    _ctx(ctx) {
		ASSERT_MSG(args.width > 0 && args.height > 0, "Swapchain width & height must both be greater than 0!");
		uint16_t width = args.width, height = args.height;

		std::vector<VkSurfaceFormatKHR> formats = [](VkPhysicalDevice vk_physical_device, VkSurfaceKHR vk_surface_khr) {
			uint32_t formatCount = 0;
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface_khr, &formatCount, nullptr));
			ASSERT_MSG(formatCount > 0, "No surface formats available for swapchain!");
			std::vector<VkSurfaceFormatKHR> result(formatCount);
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface_khr, &formatCount, result.data()));
			return result;
		}(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR);

		// get the correct colorspace and surface format
		VkSurfaceFormatKHR surfaceFormat = formats[0];
		if (args.format != VK_FORMAT_MAX_ENUM) {
			for (const auto& f: formats) {
				if (f.format == args.format && (args.colorSpace == VK_COLOR_SPACE_MAX_ENUM_KHR || f.colorSpace == args.colorSpace)) {
					surfaceFormat = f;
					break;
				}
			}
		} else if (args.colorSpace != VK_COLOR_SPACE_MAX_ENUM_KHR) {
			for (const auto& f: formats) {
				if (f.colorSpace == args.colorSpace) {
					surfaceFormat = f;
					break;
				}
			}
		}

		if (args.format != VK_FORMAT_MAX_ENUM && surfaceFormat.format != args.format) {
			LOG_SYSTEM(LogType::Warning, "Requested swapchain format is unsupported by the surface! Requested: '{}' vs Actual: '{}'", string_VkFormat(args.format), string_VkFormat(surfaceFormat.format));
		}
		if (args.colorSpace != VK_COLOR_SPACE_MAX_ENUM_KHR && surfaceFormat.colorSpace != args.colorSpace) {
			LOG_SYSTEM(LogType::Warning, "Requested swapchain color space is unsupported by the surface! Requested: '{}' vs Actual: '{}'", ColorSpaceName(args.colorSpace), ColorSpaceName(surfaceFormat.colorSpace));
		}

		VkSurfaceCapabilitiesKHR caps = {};
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR, &caps));

		const bool storageOk = [](VkPhysicalDevice vk_physical_device, VkFormat format, VkImageUsageFlags supported_usage_flags) {
			VkFormatProperties2 formatProps2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
			vkGetPhysicalDeviceFormatProperties2(vk_physical_device, format, &formatProps2);
			return (supported_usage_flags & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && (formatProps2.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
		}(_ctx._vkPhysicalDevice, surfaceFormat.format, caps.supportedUsageFlags);

		// the swapchain needs TRANSFER_DST since blitting to it requires such
		VkImageUsageFlags usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (storageOk) {
			usage_flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		}

		const std::vector<VkPresentModeKHR> presentModes = [](VkPhysicalDevice device, VkSurfaceKHR surface) {
			uint32_t modeCount = 0;
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modeCount, nullptr));
			std::vector<VkPresentModeKHR> result(modeCount);
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modeCount, result.data()));
			return result;
		}(_ctx._vkPhysicalDevice, _ctx._vkSurfaceKHR);
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
		if (args.presentMode != VK_PRESENT_MODE_MAX_ENUM_KHR) {
			if (std::ranges::find(presentModes, args.presentMode) != presentModes.end()) {
				presentMode = args.presentMode;
			} else {
				LOG_SYSTEM(LogType::Warning, "Requested present mode is unsupported by the surface, falling back to FIFO! Requested: '{}'", PresentModeName(args.presentMode));
			}
		}

		// currentExtent of UINT32_MAX means the swapchain decides the surface size
		VkExtent2D extent = caps.currentExtent;
		if (extent.width == UINT32_MAX) {
			extent.width = std::clamp<uint32_t>(width, caps.minImageExtent.width, caps.maxImageExtent.width);
			extent.height = std::clamp<uint32_t>(height, caps.minImageExtent.height, caps.maxImageExtent.height);
		}
		if (extent.width != width || extent.height != height) {
			LOG_SYSTEM(LogType::Warning, "Requested swapchain size could not be used! Requested: {} x {} vs Actual: {} x {}", width, height, extent.width, extent.height);
		}

		uint32_t minImageCount = std::max<uint32_t>(kNUM_FRAMES_IN_FLIGHT, caps.minImageCount);
		if (caps.maxImageCount != 0 && minImageCount > caps.maxImageCount) {
			LOG_SYSTEM(LogType::Warning, "Surface max image count ({}) is below frames in flight ({}), clamping!", caps.maxImageCount, (int) kNUM_FRAMES_IN_FLIGHT);
			minImageCount = caps.maxImageCount;
		}

		const uint32_t queueFamilies[] = {_ctx._graphicsQueueFamilyIndex, _ctx._presentQueueFamilyIndex};
		const bool concurrent = queueFamilies[0] != queueFamilies[1];
		const VkSwapchainCreateInfoKHR swapchainCI = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = _ctx._vkSurfaceKHR,
			.minImageCount = minImageCount,
			.imageFormat = surfaceFormat.format,
			.imageColorSpace = surfaceFormat.colorSpace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageUsage = usage_flags,
			.imageSharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = concurrent ? 2u : 0u,
			.pQueueFamilyIndices = concurrent ? queueFamilies : nullptr,
			.preTransform = caps.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = presentMode,
			.clipped = VK_TRUE,
			.oldSwapchain = VK_NULL_HANDLE,
		};
		const VkResult res = vkCreateSwapchainKHR(_ctx._vkDevice, &swapchainCI, nullptr, &_vkSwapchain);
		ASSERT_MSG(res == VK_SUCCESS, "Failed to create VkSwapchainKHR. VkResult: {}", static_cast<int>(res));

		this->_vkExtent2D = extent;
		this->_vkColorSpace = surfaceFormat.colorSpace;
		this->_vkPresentMode = presentMode;
		this->_vkImageFormat = surfaceFormat.format;

		uint32_t imageCount = 0;
		VK_CHECK(vkGetSwapchainImagesKHR(_ctx._vkDevice, _vkSwapchain, &imageCount, nullptr));
		if (imageCount > kMAX_SWAPCHAIN_IMAGES) {
			LOG_SYSTEM(LogType::FatalError, "Swapchain image count ({}) exceeds max supported swapchain images ({})!", (int) imageCount, (int) kMAX_SWAPCHAIN_IMAGES);
		}
		this->_numSwapchainImages = imageCount;
		ASSERT(_numSwapchainImages <= kMAX_SWAPCHAIN_IMAGES);
		ASSERT_MSG((caps.minImageCount <= _numSwapchainImages) && (caps.maxImageCount == 0 || _numSwapchainImages <= caps.maxImageCount), "Swapchain image count is outside the surface capabilities range!");

		std::vector<VkImage> images(imageCount);
		VK_CHECK(vkGetSwapchainImagesKHR(_ctx._vkDevice, _vkSwapchain, &imageCount, images.data()));
		std::vector<VkImageView> imageviews(imageCount);
		for (uint32_t i = 0; i < imageCount; i++) {
			const VkImageViewCreateInfo viewCI = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = images[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = _vkImageFormat,
				.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
			};
			VK_CHECK(vkCreateImageView(_ctx._vkDevice, &viewCI, nullptr, &imageviews[i]));
		}
		for (uint32_t i = 0; i < _numSwapchainImages; i++) {
			// give necessary info to texture
			AllocatedTexture image = {};
			image._isSwapchainImage = true;
			// basically the only time isOwning is false
			image._isOwning = false;
			image._vkUsageFlags = usage_flags;
			image._vkImageType = VK_IMAGE_TYPE_2D;
			image._vkImage = images[i], image._vkImageView = imageviews[i], image._vkExtent = {_vkExtent2D.width, _vkExtent2D.height, 1},
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
		for (int i = 0; i < kNUM_FRAMES_IN_FLIGHT; i++) {
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
		for (int i = 0; i < kNUM_FRAMES_IN_FLIGHT; i++) {
			vkDestroySemaphore(_ctx._vkDevice, _vkAcquireSemaphores[i], nullptr);
			if (_vkAcquireFences[i] != VK_NULL_HANDLE) {
				vkDestroyFence(_ctx._vkDevice, _vkAcquireFences[i], nullptr);
			}
		}
	}

	VkSemaphore Swapchain::acquire() {
		MYTH_PROFILER_FUNCTION_N("Swapchain Acquire");
		ASSERT_MSG(_getNextImage, "Already acquired this frame's acquire semaphore!");

		VkSemaphore vkAcquireSemaphore = _vkAcquireSemaphores[_frameIndex];
		const VkResult result = vkAcquireNextImageKHR(_ctx._vkDevice, _vkSwapchain, UINT64_MAX, vkAcquireSemaphore, VK_NULL_HANDLE, &_imageIndex);
		// out of date acquires no image and leaves the semaphore unsignaled
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			_isDirty = true;
			return VK_NULL_HANDLE;
		}
		ASSERT_MSG(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "Something went horribly wrong, vkAcquireNextImageKHR() failed!");
		if (result == VK_SUBOPTIMAL_KHR)
			_isDirty = true;
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
			if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
				return;
		}
		_getNextImage = true;
		_frameIndex = (_frameIndex + 1) % kNUM_FRAMES_IN_FLIGHT;
		MYTH_PROFILER_FRAME();
	}
} // namespace mythril
