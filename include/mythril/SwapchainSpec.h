//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include <volk.h>
#include <cstdint>

namespace mythril {
	struct SwapchainSpec {
		uint32_t width = 0;
		uint32_t height = 0;
		VkFormat format = VK_FORMAT_MAX_ENUM;
		VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
	};

	// https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html#:~:text=//%20!!-,GOOD,-CODE%20EXAMPLE%20!!
	static constexpr uint8_t kMAX_SWAPCHAIN_IMAGES = 8;

	// immediate present plays bad with triple buffering
	static constexpr uint8_t kNUM_FRAMES_IN_FLIGHT = 2;
} // namespace mythril
