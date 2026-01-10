//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "ObjectHandles.h"

#include <volk.h>

namespace mythril {
	class CTX;
	struct AllocatedTexture;

	struct SwapchainArgs {
		uint16_t width = 0;
		uint16_t height = 0;
		VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
		VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	};

	class Swapchain final {
		enum { kMAX_SWAPCHAIN_IMAGES = 16 };
	public:
		explicit Swapchain(CTX& ctx, SwapchainArgs args);
		~Swapchain();
	public: // actually used in the loop
		void acquire();
		void present();
	public:
		inline uint32_t getNumOfSwapchainImages() const { return _numSwapchainImages; }
		inline bool isDirty() const { return _isDirty; }
		const InternalTextureHandle& getCurrentSwapchainTextureHandle() const { return _swapchainTextures[_currentImageIndex]; }
		const AllocatedTexture& getCurrentSwapchainTextureObject() const;
		inline VkExtent2D getSwapchainExtent() const { return _vkExtent2D; };
	private:

		VkSwapchainKHR _vkSwapchain = VK_NULL_HANDLE;
		VkExtent2D _vkExtent2D = {};
		VkFormat _vkImageFormat = VK_FORMAT_UNDEFINED;
		// just for reading
		VkColorSpaceKHR _vkColorSpace;
		VkPresentModeKHR _vkPresentMode;

		InternalTextureHandle _swapchainTextures[kMAX_SWAPCHAIN_IMAGES] = {};
		uint64_t _timelineWaitValues[kMAX_SWAPCHAIN_IMAGES] = {}; // this HERE NEEDS FIXING
		VkSemaphore _vkAcquireSemaphores[kMAX_SWAPCHAIN_IMAGES] = {};

		uint32_t _currentImageIndex = 0;
		uint32_t _currentFrameNum = 0;
		uint32_t _numSwapchainImages = 0;
		bool _isDirty = false;
		bool _getNextImage = true;


		CTX& _ctx;
		friend class CTX;
		friend class CommandBuffer;
	};
}
