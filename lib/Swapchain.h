//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "ObjectHandles.h"

#include <volk.h>

namespace mythril {
	class CTX;
	class AllocatedTexture;

	struct SwapchainSpec
	{
		uint32_t width = 0;
		uint32_t height = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
	};

	// https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html#:~:text=//%20!!-,GOOD,-CODE%20EXAMPLE%20!!
	static constexpr uint8_t kMAX_SWAPCHAIN_IMAGES = 8;

	// immediate present plays bad with triple buffering
	static constexpr uint8_t kNUM_FRAMES_IN_FLIGHT = 2;
	class Swapchain final {
	public:
		explicit Swapchain(CTX& ctx, SwapchainSpec args);
		~Swapchain();
	public: // actually used in the loop
		VkSemaphore acquire();
		void present(VkSemaphore waitSemaphore);
	public:
		uint32_t getNumOfSwapchainImages() const { return _numSwapchainImages; }
		bool isDirty() const { return _isDirty; }
		uint32_t getCurrentImageIndex() const { return _imageIndex; }
		uint32_t getCurrentFrameIndex() const { return _frameIndex; }
		const TextureHandle& getCurrentSwapchainTextureHandle() const { return _swapchainTextures[_imageIndex]; }
		const TextureHandle& getSwapchainTextureHandle(uint32_t index) const { return _swapchainTextures[index]; }
		VkExtent2D getSwapchainExtent() const { return _vkExtent2D; }
	private:
		VkSwapchainKHR _vkSwapchain = VK_NULL_HANDLE;
		VkExtent2D _vkExtent2D = {};
		VkFormat _vkImageFormat = VK_FORMAT_UNDEFINED;
		// just for reading
		VkColorSpaceKHR _vkColorSpace;
		VkPresentModeKHR _vkPresentMode;

		// we will index by either the index of the swapchain image or the frame index,
		// these can be different
		// indexed by _imageIndex
		TextureHandle _swapchainTextures[kMAX_SWAPCHAIN_IMAGES] = {};
		uint64_t _timelineWaitValues[kMAX_SWAPCHAIN_IMAGES] = {}; // this HERE NEEDS FIXING

		// indexed by _frameIndex
		VkSemaphore _vkAcquireSemaphores[kNUM_FRAMES_IN_FLIGHT] = {};
		VkFence _vkAcquireFences[kNUM_FRAMES_IN_FLIGHT] = {}; // totally optional

		uint32_t _imageIndex = 0; // not directly altered, from vkAcquireNextImage
		uint8_t _frameIndex = 0; // 0 .. kNumFramesInFlight-1
		uint32_t _numSwapchainImages = 0; // set once and never changes!!
		bool _isDirty = false;
		bool _getNextImage = true;

		CTX& _ctx; // injection, todo remove this requirement
		friend class CTX;
		friend class CommandBuffer;
	};
}
