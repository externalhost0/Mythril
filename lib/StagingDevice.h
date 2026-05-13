//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once


#include "mythril/ObjectHandles.h"
#include "SubmitHandle.h"

#include <cstdint>
#include <span>
#include <vector>

#include <volk.h>

namespace mythril {
	class CTX;
	class AllocatedBuffer;
	class AllocatedTexture;

	constexpr uint8_t kMaxMipLevels = 16;

	class StagingDevice final {
	public:
		explicit StagingDevice(CTX& ctx);
		~StagingDevice() = default;

		StagingDevice(const StagingDevice&) = delete;
		StagingDevice& operator=(const StagingDevice&) = delete;
		BufferHandle _stagingBuffer;

	public:
		void bufferSubData(AllocatedBuffer& buffer, size_t dstOffset, size_t size, const void* data);
		struct StagedBufferCopy {
			VkDeviceSize stagingOffset = 0;
			VkDeviceSize size = 0;
			VkBuffer dstBuffer = VK_NULL_HANDLE;
			VkDeviceSize dstOffset = 0;
		};
		std::vector<StagedBufferCopy> stageBufferCopy(AllocatedBuffer& buffer, const void* data, size_t size, size_t dstOffset);
		void recordBufferCopies(VkCommandBuffer cmd, std::span<const StagedBufferCopy> copies);
		void onGraphSubmit(SubmitHandle submit);
		void resetPending();
		void
		imageData2D(AllocatedTexture& image, const VkRect2D& imageRegion, uint32_t baseMipLevel, uint32_t numMipLevels, uint32_t layerCheck, uint32_t numLayers, VkFormat format, const void* data);
		void imageData3D(AllocatedTexture& image, const VkOffset3D& offset, const VkExtent3D& extent, VkFormat format, const void* data);
		void getImageData(AllocatedTexture& image, const VkOffset3D& offset, const VkExtent3D& extent, VkImageSubresourceRange range, VkFormat format, void* outData);

	private:
		static constexpr int kStagingBufferAlignment = 16;
		struct MemoryRegionDesc {
			enum class State : uint8_t {
				Free,
				Pending,
				Busy
			};
			uint32_t offset_ = 0;
			uint32_t size_ = 0;
			SubmitHandle handle_ = {};
			State state_ = State::Free;
		};

		MemoryRegionDesc getNextFreeOffset(uint32_t size);
		void ensureStagingBufferSize(uint32_t sizeNeeded);
		void waitAndReset();
		bool isRegionFree(const MemoryRegionDesc& region) const;

	private:
		CTX& _ctx;

		uint32_t _stagingBufferSize = 0;
		uint32_t _stagingBufferCounter = 0;
		uint32_t _maxBufferSize = 0;
		const uint32_t _minBufferSize = 4u * 2048u * 2048u;
		std::vector<MemoryRegionDesc> _regions;
	};
} // namespace mythril
