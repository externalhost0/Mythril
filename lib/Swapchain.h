//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "ObjectHandles.h"

#include <volk.h>

namespace mythril {
	class CTX;
	class AllocatedTexture;

	class Swapchain final {
		enum { kMAX_SWAPCHAIN_IMAGES = 16 };
	public:
		explicit Swapchain(CTX& ctx, uint16_t width = 0, uint16_t height = 0);
		~Swapchain();
	public: // actually used in the loop
		void acquire();
		void present();
	public:
		inline uint32_t getNumOfSwapchainImages() const { return _numSwapchainImages; }
		inline bool isDirty() const { return _isDirty; }
		const AllocatedTexture& getCurrentSwapchainTexture() const;
		inline VkExtent2D getSwapchainExtent() const { return _vkExtent2D; };
	private:

		VkSwapchainKHR _vkSwapchain = VK_NULL_HANDLE;
		VkExtent2D _vkExtent2D = {};
		VkFormat _vkImageFormat = VK_FORMAT_UNDEFINED;

		InternalTextureHandle _swapchainTextures[kMAX_SWAPCHAIN_IMAGES] = {};
		uint64_t _timelineWaitValues[kMAX_SWAPCHAIN_IMAGES] = {}; // this HERE NEEDS FIXING
		VkSemaphore _vkAcquireSemaphores[kMAX_SWAPCHAIN_IMAGES] = {};

		VkImageLayout _vkCurrentImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
