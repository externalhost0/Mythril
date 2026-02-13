//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include "Invalids.h"
#include "SubmitHandle.h"

#include <volk.h>
#include <utility>

namespace mythril {

	class ImmediateCommands final {
	public:
		static constexpr uint32_t kMaxCommandBuffers = 64; // overkill lvk
		struct CommandBufferWrapper {
			VkCommandBuffer _cmdBuf = VK_NULL_HANDLE;
			VkCommandBuffer _cmdBufAllocated = VK_NULL_HANDLE;
			SubmitHandle _handle = {};
			VkFence _fence = VK_NULL_HANDLE;
			VkSemaphore _semaphore = VK_NULL_HANDLE;
			bool _isEncoding = false;
		};

		ImmediateCommands(VkDevice device, uint32_t queueFamilyIndex);
		~ImmediateCommands();

		// disallow copy and assignment
		ImmediateCommands(const ImmediateCommands&) = delete;
		ImmediateCommands& operator=(const ImmediateCommands&) = delete;
	public:
		void wait(SubmitHandle handle);

		void waitAll();
		bool isReady(SubmitHandle handle, bool fastCheckNoVulkan = false) const;
		VkFence getVkFence(SubmitHandle handle) const;
		const CommandBufferWrapper& acquire();
		SubmitHandle submit(const CommandBufferWrapper& wrapper);

		void waitSemaphore(VkSemaphore semaphore);
		void signalSemaphore(VkSemaphore semaphore, uint64_t signalValue);

		void waitTimelineSemaphore(VkSemaphore semaphore, uint64_t value);

		VkSemaphore acquireLastSubmitSemaphore() { return std::exchange(_lastSubmitSemaphore.semaphore, VK_NULL_HANDLE); }
		SubmitHandle getLastSubmitHandle() const { return _lastSubmitHandle; };
		SubmitHandle getNextSubmitHandle() const { return _nextSubmitHandle; };
	private:
		void _purge();
	private:
		VkDevice _vkDevice; // injected

		VkCommandPool _vkCommandPool = VK_NULL_HANDLE;

		VkSemaphoreSubmitInfo _lastSubmitSemaphore = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
		};
		VkSemaphoreSubmitInfo _waitSemaphore = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
		};
		VkSemaphoreSubmitInfo _signalSemaphore = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
		};

		VkQueue _vkQueue = VK_NULL_HANDLE;
		uint32_t _queueFamilyIndex = Invalid<uint32_t>;

		CommandBufferWrapper _buffers[kMaxCommandBuffers];

		SubmitHandle _lastSubmitHandle = SubmitHandle();
		SubmitHandle _nextSubmitHandle = SubmitHandle();

		uint32_t _numAvailableCommandBuffers = kMaxCommandBuffers;
		uint32_t _submitCounter = 1;

		friend class CommandBuffer;
		friend class Swapchain;
		friend class StagingDevice;
	};

}