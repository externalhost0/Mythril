//
// Created by Hayden Rivas on 10/6/25.
//

#include "StagingDevice.h"
#include "CTX.h"
#include "vkutil.h"


namespace mythril {
	StagingDevice::StagingDevice(CTX& ctx) : _ctx(ctx) {
		const VkPhysicalDeviceLimits& limits = _ctx.getPhysicalDeviceProperties10().limits;
		// use default value of 128Mb clamped to the max limits
		_maxBufferSize = std::min(limits.maxStorageBufferRange, 128u * 1024u * 1024u);
		ASSERT_MSG(_minBufferSize <= _maxBufferSize, "Min buffer size MUST BE smaller than or equal to max buffer size!");
	}
	void StagingDevice::bufferSubData(AllocatedBuffer& buffer, size_t dstOffset, size_t size, const void* data) {
		if (buffer.isMapped()) {
			buffer.bufferSubData(_ctx, dstOffset, size, data);
			return;
		}
		AllocatedBuffer* stagingBuffer = _ctx._bufferPool.get(_stagingBuffer);
		ASSERT(stagingBuffer);

		while (size) {
			// get next staging buffer free offset
			MemoryRegionDesc desc = getNextFreeOffset((uint32_t)size);
			const uint32_t chunkSize = std::min((uint32_t)size, desc.size_);

			// copy data into staging buffer
			stagingBuffer->bufferSubData(_ctx, desc.offset_, chunkSize, data);

			// do the transfer
			const VkBufferCopy copy = {
					.srcOffset = desc.offset_,
					.dstOffset = dstOffset,
					.size = chunkSize,
			};

			const ImmediateCommands::CommandBufferWrapper& wrapper = _ctx._imm->acquire();
			vkCmdCopyBuffer(wrapper._cmdBuf, stagingBuffer->_vkBuffer, buffer._vkBuffer, 1, &copy);
			VkBufferMemoryBarrier barrier = {
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.dstAccessMask = 0,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.buffer = buffer._vkBuffer,
					.offset = dstOffset,
					.size = chunkSize,
			};
			VkPipelineStageFlags dstMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			if (buffer._vkUsageFlags & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) {
				dstMask |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
				barrier.dstAccessMask |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
			}
			if (buffer._vkUsageFlags & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
				dstMask |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
				barrier.dstAccessMask |= VK_ACCESS_INDEX_READ_BIT;
			}
			if (buffer._vkUsageFlags & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
				dstMask |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
				barrier.dstAccessMask |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
			}
			if (buffer._vkUsageFlags & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) {
				dstMask |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
				barrier.dstAccessMask |= VK_ACCESS_MEMORY_READ_BIT;
			}
			vkCmdPipelineBarrier(wrapper._cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, dstMask, VkDependencyFlags{}, 0, nullptr, 1, &barrier, 0, nullptr);
			desc.handle_ = _ctx._imm->submit(wrapper);
			_regions.push_back(desc);

			size -= chunkSize;
			data = (uint8_t*)data + chunkSize;
			dstOffset += chunkSize;
		}
	}
	void StagingDevice::imageData3D(AllocatedTexture& image, const VkOffset3D& offset, const VkExtent3D& extent, VkFormat format, const void* data) {
		ASSERT_MSG(image._numLevels == 1, "Can handle only 3D images with exactly 1 mip-level");
		ASSERT_MSG((offset.x == 0) && (offset.y == 0) && (offset.z == 0), "Can upload only full-size 3D images");
		const uint32_t storageSize = extent.width * extent.height * extent.depth * vkutil::GetBytesPerPixel(format);

		ensureStagingBufferSize(storageSize);

		ASSERT_MSG(storageSize <= _stagingBufferSize, "No support for copying image in multiple smaller chunk sizes");

		// get next staging buffer free offset
		MemoryRegionDesc desc = getNextFreeOffset(storageSize);

		// No support for copying image in multiple smaller chunk sizes.
		// If we get smaller buffer size than storageSize, we will wait for GPU idle and get a bigger chunk.
		if (desc.size_ < storageSize) {
			waitAndReset();
			desc = getNextFreeOffset(storageSize);
		}

		ASSERT(desc.size_ >= storageSize);

		AllocatedBuffer* stagingBuffer = _ctx._bufferPool.get(_stagingBuffer);

		// 1. Copy the pixel data into the host visible staging buffer
		stagingBuffer->bufferSubData(_ctx, desc.offset_, storageSize, data);

		const ImmediateCommands::CommandBufferWrapper& wrapper = _ctx._imm->acquire();

		// 1. Transition initial image layout into TRANSFER_DST_OPTIMAL
		vkutil::ImageMemoryBarrier2(wrapper._cmdBuf,
									image._vkImage,
									vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .access = VK_ACCESS_2_NONE},
									vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
									VK_IMAGE_LAYOUT_UNDEFINED,
									VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

		// 2. Copy the pixel data from the staging buffer into the image
		const VkBufferImageCopy copy = {
				.bufferOffset = desc.offset_,
				.bufferRowLength = 0,
				.bufferImageHeight = 0,
				.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				.imageOffset = offset,
				.imageExtent = extent,
		};
		vkCmdCopyBufferToImage(wrapper._cmdBuf, stagingBuffer->_vkBuffer, image._vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// 3. Transition TRANSFER_DST_OPTIMAL into SHADER_READ_ONLY_OPTIMAL
		vkutil::ImageMemoryBarrier2(
				wrapper._cmdBuf,
				image._vkImage,
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

		image._vkCurrentImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		desc.handle_ = _ctx._imm->submit(wrapper);
		_regions.push_back(desc);
	}
	void StagingDevice::imageData2D(AllocatedTexture& image,
									const VkRect2D& imageRegion,
									uint32_t baseMipLevel,
									uint32_t numMipLevels,
									uint32_t layerCheck,
									uint32_t numLayers,
									VkFormat format,
									const void* data) {
		ASSERT(numMipLevels <= kMaxMipLevels);

		// divide the width and height by 2 until we get to the size of level 'baseMipLevel'
		uint32_t width = image._vkExtent.width >> baseMipLevel;
		uint32_t height = image._vkExtent.height >> baseMipLevel;

		// not sure
		ASSERT_MSG(!imageRegion.offset.x && !imageRegion.offset.y && imageRegion.extent.width == width && imageRegion.extent.height == height,
				   "Uploading mip-levels with an image region that is smaller than the base mip level is not supported!");

		// find the storage size for all mip-levels being uploaded
		uint32_t layerStorageSize = 0;
		for (uint32_t i = 0; i < numMipLevels; ++i) {
			layerStorageSize += vkutil::GetTextureBytesPerLayer(image._vkExtent.width, image._vkExtent.height, format, i);
			width = std::max(1u, width >> 1);
			height = std::max(1u, height >> 1);
//			width = width <= 1 ? 1 : width >> 1;
//			height = height <= 1 ? 1 : height >> 1;
		}
		const uint32_t storageSize = layerStorageSize * numLayers;

		ensureStagingBufferSize(storageSize);
		ASSERT(storageSize <= _stagingBufferSize);
		MemoryRegionDesc desc = getNextFreeOffset(storageSize);
		// No support for copying image in multiple smaller chunk sizes. If we get smaller buffer size than storageSize, we will wait for GPU idle
		// and get bigger chunk.
		if (desc.size_ < storageSize) {
			waitAndReset();
			desc = getNextFreeOffset(storageSize);
		}
		ASSERT(desc.size_ >= storageSize);

		const ImmediateCommands::CommandBufferWrapper& wrapper = _ctx._imm->acquire();

		AllocatedBuffer* stagingBuffer = _ctx._bufferPool.get(_stagingBuffer);
		stagingBuffer->bufferSubData(_ctx, desc.offset_, storageSize, data);

		uint32_t offset = 0;
		const uint32_t numPlanes = vkutil::GetNumImagePlanes(image._vkFormat);
		if (numPlanes > 1) {
			ASSERT(layerCheck == 0 && baseMipLevel == 0);
			ASSERT(numLayers == 1 && numMipLevels == 1);
			ASSERT(imageRegion.offset.x == 0 && imageRegion.offset.y == 0);
			ASSERT(image._vkImageType == VK_IMAGE_TYPE_2D);
			ASSERT(image._vkExtent.width == imageRegion.extent.width && image._vkExtent.height == imageRegion.extent.height);
		}
		VkImageAspectFlags imageAspect = VK_IMAGE_ASPECT_COLOR_BIT;
		if (numPlanes == 2) {
			imageAspect = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
		}
		if (numPlanes == 3) {
			imageAspect = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;
		}

		// https://registry.khronos.org/KTX/specs/1.0/ktxspec.v1.html
		for (uint32_t mipLevel = 0; mipLevel < numMipLevels; mipLevel++) {
			for (uint32_t layer = 0; layer != numLayers; layer++) {
				const uint32_t currentMipLevel = baseMipLevel + mipLevel;

				// 1. Transition initial image layout into TRANSFER_DST_OPTIMAL
				vkutil::ImageMemoryBarrier2(wrapper._cmdBuf,
											image._vkImage,
											vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .access = VK_ACCESS_2_NONE},
											vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
											VK_IMAGE_LAYOUT_UNDEFINED,
											VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											VkImageSubresourceRange{imageAspect, currentMipLevel, 1, layer, 1});

				// 2. Copy the pixel data from the staging buffer into the image
				uint32_t planeOffset = 0;
				for (uint32_t plane = 0; plane != numPlanes; plane++) {
					const VkExtent2D extent = vkutil::GetImagePlaneExtent(
							{
									.width = std::max(1u, imageRegion.extent.width >> mipLevel),
									.height = std::max(1u, imageRegion.extent.height >> mipLevel),
							},
							format,
							plane);
					const VkRect2D region = {
							.offset = {
									.x = imageRegion.offset.x >> mipLevel,
									.y = imageRegion.offset.y >> mipLevel
							},
							.extent = extent,
					};
					const VkBufferImageCopy copy = {
							// the offset for this level is at the start of all mip-levels plus the size of all previous mip-levels being uploaded
							.bufferOffset = desc.offset_ + offset + planeOffset,
							.bufferRowLength = 0,
							.bufferImageHeight = 0,
							.imageSubresource =
							VkImageSubresourceLayers{numPlanes > 1 ? VK_IMAGE_ASPECT_PLANE_0_BIT << plane : imageAspect, currentMipLevel, layer, 1},
							.imageOffset = {.x = region.offset.x, .y = region.offset.y, .z = 0},
							.imageExtent = {.width = region.extent.width, .height = region.extent.height, .depth = 1u},
					};
					vkCmdCopyBufferToImage(wrapper._cmdBuf, stagingBuffer->_vkBuffer, image._vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
					planeOffset += vkutil::GetTextureBytesPerPlane(imageRegion.extent.width, imageRegion.extent.height, format, plane);
				}

				// 3. Transition TRANSFER_DST_OPTIMAL into SHADER_READ_ONLY_OPTIMAL
				vkutil::ImageMemoryBarrier2(
						wrapper._cmdBuf,
						image._vkImage,
						vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT},
						vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VkImageSubresourceRange{imageAspect, currentMipLevel, 1, layer, 1});

				offset += vkutil::GetTextureBytesPerLayer(imageRegion.extent.width, imageRegion.extent.height, format, currentMipLevel);
			}
		}
		image._vkCurrentImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		desc.handle_ = _ctx._imm->submit(wrapper);
		_regions.push_back(desc);
	}
	void StagingDevice::getImageData(AllocatedTexture& image, const VkOffset3D& offset, const VkExtent3D& extent, VkImageSubresourceRange range, VkFormat format, void* outData) {
		ASSERT(image.getImageLayout() != VK_IMAGE_LAYOUT_UNDEFINED);
		ASSERT(range.layerCount == 1);

		const uint32_t storageSize = extent.width * extent.height * extent.depth * vkutil::GetBytesPerPixel(format);

		ensureStagingBufferSize(storageSize);

		ASSERT(storageSize <= _stagingBufferSize);

		// get next staging buffer free offset
		MemoryRegionDesc desc = getNextFreeOffset(storageSize);

		// No support for copying image in multiple smaller chunk sizes.
		// If we get smaller buffer size than storageSize, we will wait for GPU idle and get a bigger chunk.
		if (desc.size_ < storageSize) {
			waitAndReset();
			desc = getNextFreeOffset(storageSize);
		}

		ASSERT(desc.size_ >= storageSize);

		AllocatedBuffer* stagingBuffer = _ctx._bufferPool.get(_stagingBuffer);

		const ImmediateCommands::CommandBufferWrapper& wrapper1 = _ctx._imm->acquire();

		// 1. Transition to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
		vkutil::ImageMemoryBarrier2(
				wrapper1._cmdBuf,
				image._vkImage,
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT},
				image.getImageLayout(),
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				range);

		// 2.  Copy the pixel data from the image into the staging buffer
		const VkBufferImageCopy copy = {
				.bufferOffset = desc.offset_,
				.bufferRowLength = 0,
				.bufferImageHeight = extent.height,
				.imageSubresource =
				VkImageSubresourceLayers{
						.aspectMask = range.aspectMask,
						.mipLevel = range.baseMipLevel,
						.baseArrayLayer = range.baseArrayLayer,
						.layerCount = range.layerCount,
				},
				.imageOffset = offset,
				.imageExtent = extent,
		};
		vkCmdCopyImageToBuffer(wrapper1._cmdBuf, image._vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer->_vkBuffer, 1, &copy);

		desc.handle_ = _ctx._imm->submit(wrapper1);
		_regions.push_back(desc);

		waitAndReset();

		if (!stagingBuffer->_isCoherentMemory) {
			stagingBuffer->invalidateMappedMemory(_ctx, desc.offset_, desc.size_);
		}

		// 3. Copy data from staging buffer into data
		memcpy(outData, stagingBuffer->getMappedPtr() + desc.offset_, storageSize);

		// 4. Transition back to the initial image layout
		const ImmediateCommands::CommandBufferWrapper& wrapper2 = _ctx._imm->acquire();

		vkutil::ImageMemoryBarrier2(
				wrapper2._cmdBuf,
				image._vkImage,
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT},
				vkutil::StageAccess{.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT},
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				image.getImageLayout(),
				range);

		_ctx._imm->wait(_ctx._imm->submit(wrapper2));
	}

	void StagingDevice::ensureStagingBufferSize(uint32_t sizeNeeded) {
		assert(kStagingBufferAlignment != 0);
		const uint32_t alignedSize = std::max(vkutil::GetAlignedSize(sizeNeeded, kStagingBufferAlignment), _minBufferSize);
		sizeNeeded = (alignedSize < _maxBufferSize) ? alignedSize : _maxBufferSize;
		if (!_stagingBuffer.empty()) {
			const bool isEnoughSize = sizeNeeded <= _stagingBufferSize;
			const bool isMaxSize = _stagingBufferSize == _maxBufferSize;
			if (isEnoughSize || isMaxSize) {
				return;
			}
		}

		// FIXME: buffer allocation with vma warning is from here, no idea how to solve
		// seems to be gone
		waitAndReset();
		// deallocate the previous staging buffer
		_ctx.destroy(_stagingBuffer);
		// if the combined size of the new staging buffer and the existing one is larger than the limit imposed by some architectures on buffers
		// that are device and host visible, we need to wait for the current buffer to be destroyed before we can allocate a new one
		if ((sizeNeeded + _stagingBufferSize) > _maxBufferSize) {
			_ctx.waitDeferredTasks();
		}
		_stagingBufferSize = sizeNeeded;
		{
			AllocatedBuffer obj = _ctx.createBufferImpl(_stagingBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
			snprintf(obj._debugName, sizeof(obj._debugName) - 1, "staging buffer %u", _stagingBufferCounter++);
			vmaSetAllocationName(_ctx._vmaAllocator, obj._vmaAllocation, obj._debugName);
			_stagingBuffer = _ctx._bufferPool.create(std::move(obj));
		}

		ASSERT(!_stagingBuffer.empty());

		_regions.clear();
		_regions.push_back({0, _stagingBufferSize, SubmitHandle()});
	}
	StagingDevice::MemoryRegionDesc StagingDevice::getNextFreeOffset(uint32_t size) {
		const uint32_t requestedAlignedSize = vkutil::GetAlignedSize(size, kStagingBufferAlignment);
		ensureStagingBufferSize(requestedAlignedSize);

		ASSERT(!_regions.empty());

		// if we can't find an available region that is big enough to store requestedAlignedSize, return whatever we could find, which will be
		// stored in bestNextIt
		auto bestNextIt = _regions.begin();

		for (auto it = _regions.begin(); it != _regions.end(); ++it) {
			if (_ctx._imm->isReady(it->handle_)) {
				// This region is free, but is it big enough?
				if (it->size_ >= requestedAlignedSize) {
					// It is big enough!
					const uint32_t unusedSize = it->size_ - requestedAlignedSize;
					const uint32_t unusedOffset = it->offset_ + requestedAlignedSize;

					// Prepare return value
					const auto result = MemoryRegionDesc{it->offset_, requestedAlignedSize, SubmitHandle()};

					// Perform the cleanup that was in SCOPE_EXIT
					_regions.erase(it);
					if (unusedSize > 0) {
						_regions.insert(_regions.begin(), {unusedOffset, unusedSize, SubmitHandle()});
					}

					return result;
				}

				// Cache the largest available region that isn't as big as the one we're looking for
				if (it->size_ > bestNextIt->size_) {
					bestNextIt = it;
				}
			}
		}
		// we found a region that is available that is smaller than the requested size. It's the best we can do
		// we found a region that is available that is smaller than the requested size. It's the best we can do
		if (bestNextIt != _regions.end() && _ctx._imm->isReady(bestNextIt->handle_)) {
			const auto result = MemoryRegionDesc{bestNextIt->offset_, bestNextIt->size_, SubmitHandle()};
			// Perform the cleanup that was in SCOPE_EXIT
			_regions.erase(bestNextIt);
			return result;
		}
		// nothing was available. Let's wait for the entire staging buffer to become free
		waitAndReset();

		// waitAndReset() adds a region that spans the entire buffer. Since we'll be using part of it, we need to replace it with a used block and
		// an unused portion
		_regions.clear();
		// store the unused size in the deque first...
		const uint32_t unusedSize = _stagingBufferSize > requestedAlignedSize ? _stagingBufferSize - requestedAlignedSize : 0;

		if (unusedSize) {
			const uint32_t unusedOffset = _stagingBufferSize - unusedSize;
			_regions.insert(_regions.begin(), {unusedOffset, unusedSize, SubmitHandle()});
		}
		// ...and then return the smallest free region that can hold the requested size
		return {
				.offset_ = 0,
				.size_ = _stagingBufferSize - unusedSize,
				.handle_ = SubmitHandle(),
		};
	}
	void StagingDevice::waitAndReset() {
		for (const MemoryRegionDesc& r : _regions) {
			_ctx._imm->wait(r.handle_);
		}
		_regions.clear();
		_regions.push_back({0, _stagingBufferSize, SubmitHandle()});
	}

}
