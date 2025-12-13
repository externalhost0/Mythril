//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once


#include "SubmitHandle.h"
#include "ObjectHandles.h"

#include <cstdint>
#include <vector>

#include <volk.h>

namespace mythril {
	class CTX;
	struct AllocatedBuffer;
	struct AllocatedTexture;

	constexpr uint8_t kMaxMipLevels = 16;

	class StagingDevice final {
	public:
		explicit StagingDevice(CTX& ctx);
		~StagingDevice() = default;

		StagingDevice(const StagingDevice&) = delete;
		StagingDevice& operator=(const StagingDevice&) = delete;
		InternalBufferHandle _stagingBuffer;
	public:
		void bufferSubData(AllocatedBuffer& buffer, size_t dstOffset, size_t size, const void* data);
		void imageData2D(AllocatedTexture& image,
						 const VkRect2D& imageRegion,
						 uint32_t baseMipLevel,
						 uint32_t numMipLevels,
						 uint32_t layerCheck,
						 uint32_t numLayers,
						 VkFormat format,
						 const void* data);
		void imageData3D(AllocatedTexture& image, const VkOffset3D& offset, const VkExtent3D& extent, VkFormat format, const void* data);
		void getImageData(AllocatedTexture& image,
						  const VkOffset3D& offset,
						  const VkExtent3D& extent,
						  VkImageSubresourceRange range,
						  VkFormat format,
						  void* outData);
	private:
		static constexpr int kStagingBufferAlignment = 16;
		struct MemoryRegionDesc {
			uint32_t offset_ = 0;
			uint32_t size_ = 0;
			SubmitHandle handle_ = {};
		};

		MemoryRegionDesc getNextFreeOffset(uint32_t size);
		void ensureStagingBufferSize(uint32_t sizeNeeded);
		void waitAndReset();
	private:
		CTX& _ctx;

		uint32_t _stagingBufferSize = 0;
		uint32_t _stagingBufferCounter = 0;
		uint32_t _maxBufferSize = 0;
		const uint32_t _minBufferSize = 4u * 2048u * 2048u;
		std::vector<MemoryRegionDesc> _regions;
	};
}