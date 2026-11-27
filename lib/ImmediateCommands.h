//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include "HelperMacros.h"
#include "mythril/SubmitHandle.h"

#include <array>
#include <utility>
#include <volk.h>

namespace mythril {
    namespace detail {
        struct CommandBufferWrapper {
            VkCommandBuffer _cmdBuf = VK_NULL_HANDLE;
            VkCommandBuffer _cmdBufAllocated = VK_NULL_HANDLE;
            SubmitHandle _handle = {};
            VkFence _fence = VK_NULL_HANDLE;
            VkSemaphore _semaphore = VK_NULL_HANDLE;
            bool _isEncoding = false;
        };
    } // namespace detail

    class ImmediateCommands final {
    public:
        static constexpr uint32_t kMaxCommandBuffers = 64; // overkill lvk

        ImmediateCommands(VkDevice device, uint32_t queueFamilyIndex, VkQueue vk_queue = VK_NULL_HANDLE);
        ~ImmediateCommands();

        // disallow copy and assignment
        ImmediateCommands(const ImmediateCommands&) = delete;
        ImmediateCommands& operator=(const ImmediateCommands&) = delete;

    public:
        void wait(SubmitHandle handle);

        void waitAll();
        bool isReady(SubmitHandle handle, bool fastCheckNoVulkan = false) const;
        VkFence getVkFence(SubmitHandle handle) const;
        const detail::CommandBufferWrapper& acquire();
        SubmitHandle submit(const detail::CommandBufferWrapper& wrapper);

        // binary, only the swapchain acquire uses this
        void waitSemaphore(VkSemaphore semaphore);
        void signalSemaphore(VkSemaphore semaphore, uint64_t signalValue);
        void waitTimeline(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stageMask);

        VkSemaphore getTimeline() const { return _timeline; }
        uint64_t getTimelineValue() const { return _timelineValue; }
        uint32_t getQueueFamilyIndex() const { return _queueFamilyIndex; }
        VkQueue getQueue() const { return _vkQueue; }

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
            .stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        };
        // only the swapchain acquire uses this, and swapchain images are only written by attachments or transfers (never bindless)
        VkSemaphoreSubmitInfo _waitSemaphore = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        };
        // the frame timeline plus external signals from the render graph
        std::array<VkSemaphoreSubmitInfo, 4> _signals{};
        uint32_t _numSignals = 0;
        // one per other queue is enough, a batch waits on each queue at most once
        std::array<VkSemaphoreSubmitInfo, 4> _timelineWaits{};
        uint32_t _numTimelineWaits = 0;
        VkSemaphore _timeline = VK_NULL_HANDLE;
        uint64_t _timelineValue = 0;

        VkQueue _vkQueue = VK_NULL_HANDLE;
        uint32_t _queueFamilyIndex = MYTHRIL_INVALID_U32;

        detail::CommandBufferWrapper _buffers[kMaxCommandBuffers];

        SubmitHandle _lastSubmitHandle = SubmitHandle();
        SubmitHandle _nextSubmitHandle = SubmitHandle();

        uint32_t _numAvailableCommandBuffers = kMaxCommandBuffers;
        uint32_t _submitCounter = 1;

        friend class CommandBuffer;
        friend class Swapchain;
        friend class StagingDevice;
    };

} // namespace mythril
