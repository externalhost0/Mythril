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

	class Swapchain final {
	public:
		enum { kMAX_SWAPCHAIN_IMAGES = 16 };
		explicit Swapchain(CTX& ctx, SwapchainSpec args);
		~Swapchain();
	public: // actually used in the loop
		void acquire();
		void present();
	public:
		uint32_t getNumOfSwapchainImages() const { return _numSwapchainImages; }
		bool isDirty() const { return _isDirty; }
		uint32_t getCurrentImageIndex() const { return _currentImageIndex; }
		const TextureHandle& getCurrentSwapchainTextureHandle() const { return _swapchainTextures[_currentImageIndex]; }
		const TextureHandle& getSwapchainTextureHandle(uint32_t index) const { return _swapchainTextures[index]; }
		VkExtent2D getSwapchainExtent() const { return _vkExtent2D; };
	private:
		VkSwapchainKHR _vkSwapchain = VK_NULL_HANDLE;
		VkExtent2D _vkExtent2D = {};
		VkFormat _vkImageFormat = VK_FORMAT_UNDEFINED;
		// just for reading
		VkColorSpaceKHR _vkColorSpace;
		VkPresentModeKHR _vkPresentMode;

		TextureHandle _swapchainTextures[kMAX_SWAPCHAIN_IMAGES] = {};
		uint64_t _timelineWaitValues[kMAX_SWAPCHAIN_IMAGES] = {}; // this HERE NEEDS FIXING
		VkSemaphore _vkAcquireSemaphores[kMAX_SWAPCHAIN_IMAGES] = {};

		VkFence _vkAcquireFences[kMAX_SWAPCHAIN_IMAGES] = {};

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
