//
// Created by Hayden Rivas on 10/6/25.
//
#include "ImmediateCommands.h"
#include "Logger.h"
#include "vkinfo.h"

namespace mythril {
	ImmediateCommands::ImmediateCommands(VkDevice device, uint32_t queueFamilyIndex) : _vkDevice(device), _queueFamilyIndex(queueFamilyIndex) {
		// just use the family index to get our queue vulkan object
		vkGetDeviceQueue(device, queueFamilyIndex, 0, &_vkQueue);

		// create command pool
		const VkCommandPoolCreateInfo command_pool_ci = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = queueFamilyIndex,
		};
		VK_CHECK(vkCreateCommandPool(device, &command_pool_ci, nullptr, &_vkCommandPool));

		// create command buffers
		const VkCommandBufferAllocateInfo command_buffer_ai = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = _vkCommandPool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
		};

		for (uint32_t i = 0; i != kMaxCommandBuffers; i++) {
			CommandBufferWrapper& buf = _buffers[i];

			// make semaphore
			const VkSemaphoreCreateInfo semaphore_ci = vkinfo::CreateSemaphoreInfo();
			VkSemaphore semaphore = VK_NULL_HANDLE;
			VK_CHECK(vkCreateSemaphore(_vkDevice, &semaphore_ci, nullptr, &semaphore));
			buf._semaphore = semaphore;
			// make fence
			const VkFenceCreateInfo fence_ci = vkinfo::CreateFenceInfo();
			VkFence fence = VK_NULL_HANDLE;
			VK_CHECK(vkCreateFence(_vkDevice, &fence_ci, nullptr, &fence));
			buf._fence = fence;

			VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_ai, &buf._cmdBufAllocated));
			_buffers[i]._handle.bufferIndex_ = i;
		}
	}
	ImmediateCommands::~ImmediateCommands() {
		waitAll();
		for (CommandBufferWrapper& buf : _buffers) {
			// lifetimes of all VkFence objects are managed explicitly we do not use deferredTask() for them
			vkDestroyFence(_vkDevice, buf._fence, nullptr);
			vkDestroySemaphore(_vkDevice, buf._semaphore, nullptr);
		}
		vkDestroyCommandPool(_vkDevice, _vkCommandPool, nullptr);
	}

	void ImmediateCommands::wait(SubmitHandle handle) {
		if (handle.empty()) {
			vkDeviceWaitIdle(_vkDevice);
			return;
		}
		if (isReady(handle)) {
			return;
		}
		if (_buffers[handle.bufferIndex_]._isEncoding) {
			// we are waiting for a buffer which has not been submitted - this is probably a logic error somewhere in the calling code
			return;
		}
		VK_CHECK(vkWaitForFences(_vkDevice, 1, &_buffers[handle.bufferIndex_]._fence, VK_TRUE, UINT64_MAX));
		_purge();
	}
	void ImmediateCommands::waitAll() {
		VkFence fences[kMaxCommandBuffers];
		uint32_t numFences = 0;
		for (const CommandBufferWrapper& buf : _buffers) {
			if (buf._cmdBuf != VK_NULL_HANDLE && !buf._isEncoding) {
				fences[numFences++] = buf._fence;
			}
		}
		if (numFences) {
			VK_CHECK(vkWaitForFences(_vkDevice, numFences, fences, VK_TRUE, UINT64_MAX));
		}
		_purge();
	}

	void ImmediateCommands::_purge() {
		const uint32_t numBuffers = kMaxCommandBuffers;

		for (uint32_t i = 0; i != numBuffers; i++) {
			// always start checking with the oldest submitted buffer, then wrap around
			CommandBufferWrapper& buf = _buffers[(i + _lastSubmitHandle.bufferIndex_ + 1) % numBuffers];
			if (buf._cmdBuf == VK_NULL_HANDLE || buf._isEncoding) {
				continue;
			}
			const VkResult result = vkWaitForFences(_vkDevice, 1, &buf._fence, VK_TRUE, 0);

			if (result == VK_SUCCESS) {
				VK_CHECK(vkResetCommandBuffer(buf._cmdBuf, VkCommandBufferResetFlags{0}));
				VK_CHECK(vkResetFences(_vkDevice, 1, &buf._fence));
				buf._cmdBuf = VK_NULL_HANDLE;
				_numAvailableCommandBuffers++;
			} else {
				if (result != VK_TIMEOUT) {
					ASSERT(result);
				}
			}
		}
	}
	bool ImmediateCommands::isReady(SubmitHandle handle, bool fastCheckNoVulkan) const {
		if (handle.empty()) {
			// a null handle
			return true;
		}
		const CommandBufferWrapper& buf = _buffers[handle.bufferIndex_];
		if (buf._cmdBuf == VK_NULL_HANDLE) {
			// already recycled and not yet reused
			return true;
		}
		if (buf._handle.submitId_ != handle.submitId_) {
			// already recycled and reused by another command buffer
			return true;
		}
		if (fastCheckNoVulkan) {
			// do not ask the Vulkan API about it, just let it retire naturally (when submitId for this bufferIndex gets incremented)
			return false;
		}
		return vkWaitForFences(_vkDevice, 1, &buf._fence, VK_TRUE, 0) == VK_SUCCESS;
	}
	SubmitHandle ImmediateCommands::submit(const CommandBufferWrapper& wrapper) {
		ASSERT_MSG(wrapper._isEncoding, "Command buffer must be encoding!");
		VK_CHECK(vkEndCommandBuffer(wrapper._cmdBuf));

		VkSemaphoreSubmitInfo waitSemaphores[] = {{}, {}};
		uint32_t numWaitSemaphores = 0;
		if (_waitSemaphore.semaphore) {
			waitSemaphores[numWaitSemaphores++] = _waitSemaphore;
		}
		if (_lastSubmitSemaphore.semaphore) {
			waitSemaphores[numWaitSemaphores++] = _lastSubmitSemaphore;
		}
		VkSemaphoreSubmitInfo signalSemaphores[] = {
				VkSemaphoreSubmitInfo{
						.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
						.semaphore = wrapper._semaphore,
						.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
				{},
		};
		uint32_t numSignalSemaphores = 1;
		if (_signalSemaphore.semaphore) {
			signalSemaphores[numSignalSemaphores++] = _signalSemaphore;
		}

		const VkCommandBufferSubmitInfo command_buffer_si = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = wrapper._cmdBuf,
		};
		const VkSubmitInfo2 si = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				.waitSemaphoreInfoCount = numWaitSemaphores,
				.pWaitSemaphoreInfos = waitSemaphores,
				.commandBufferInfoCount = 1u,
				.pCommandBufferInfos = &command_buffer_si,
				.signalSemaphoreInfoCount = numSignalSemaphores,
				.pSignalSemaphoreInfos = signalSemaphores,
		};
		VK_CHECK(vkQueueSubmit2KHR(_vkQueue, 1u, &si, wrapper._fence));

		_lastSubmitSemaphore.semaphore = wrapper._semaphore;
		_lastSubmitHandle = wrapper._handle;
		_waitSemaphore.semaphore = VK_NULL_HANDLE;
		_signalSemaphore.semaphore = VK_NULL_HANDLE;

		// reset
		const_cast<CommandBufferWrapper&>(wrapper)._isEncoding = false;
		_submitCounter++;

		if (!_submitCounter) {
			// skip the 0 value - when uint32_t wraps around (null SubmitHandle)
			_submitCounter++;
		}
		return _lastSubmitHandle;
	}
	void ImmediateCommands::waitSemaphore(VkSemaphore semaphore) {
		ASSERT_MSG(_waitSemaphore.semaphore == VK_NULL_HANDLE, "Current wait semaphore is VK_NULL_HANDLE!");
		_waitSemaphore.semaphore = semaphore;
	}
	void ImmediateCommands::signalSemaphore(VkSemaphore semaphore, uint64_t signalValue) {
		ASSERT_MSG(_signalSemaphore.semaphore == VK_NULL_HANDLE, "Current signal semaphore is VK_NULL_HANDLE!");
		_signalSemaphore.semaphore = semaphore;
		_signalSemaphore.value = signalValue;
	}
	VkFence ImmediateCommands::getVkFence(SubmitHandle handle) const {
		if (handle.empty()) {
			return VK_NULL_HANDLE;
		}
		return _buffers[handle.bufferIndex_]._fence;
	}
	const ImmediateCommands::CommandBufferWrapper& ImmediateCommands::acquire() {
		if (!_numAvailableCommandBuffers) {
			_purge();
		}
		while (!_numAvailableCommandBuffers) {
			LOG_USER(LogType::Info, "Waiting for command buffers..\n");
			_purge();
		}
		ImmediateCommands::CommandBufferWrapper* current = nullptr;
		// we are ok with any available buffer
		for (CommandBufferWrapper& buf : _buffers) {
			if (buf._cmdBuf == VK_NULL_HANDLE) {
				current = &buf;
				break;
			}
		}
		ASSERT_MSG(_numAvailableCommandBuffers, "No available command buffers");
		ASSERT_MSG(current, "No available command buffers");
		ASSERT_MSG(current->_cmdBufAllocated != VK_NULL_HANDLE, "Command buffer allocated is NULL_HANDLE");

		current->_handle.submitId_ = _submitCounter;
		_numAvailableCommandBuffers--;

		current->_cmdBuf = current->_cmdBufAllocated;
		current->_isEncoding = true;
		const VkCommandBufferBeginInfo bi = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		VK_CHECK(vkBeginCommandBuffer(current->_cmdBuf, &bi));
		_nextSubmitHandle = current->_handle;
		return *current;
	}
}