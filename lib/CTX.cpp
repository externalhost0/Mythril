//
// Created by Hayden Rivas on 10/6/25.
//

#include "CTX.h"
#include "vkutil.h"
#include "Logger.h"
#include "PipelineBuilder.h"
#include "GraphicsPipeline.h"
#include "Plugins.h"
#include "mythril/CTXBuilder.h"

#include <iostream>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <slang/slang.h>
#include <slang/slang-cpp-types.h>
#include <slang/slang-com-ptr.h>
#include <slang/slang-cpp-types-core.h>

namespace mythril {
	// we dont use combined image samplers
	enum Bindings {
		kGlobalBinding = 0,

		kSamplerBinding = 0,
		kTextureBinding = 1,
		kStorageImageBinding = 2,

		kNumOfBindings,
	};
	// https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/03-convenience-features.html#descriptorhandle-for-bindless-descriptor-access:~:text=None%20provides%20the%20following%20bindings%20for%20descriptor%20types%3A-,Enum,-Value
	namespace BindlessSpaceIndex {
		enum Type : uint8_t {
			kSampler = 0,
//			kCombinedImageSampler = 1,
			kSampledImage = 2,
			kStorageImage = 3,
//			kUniformTexelBuffer = 2,
//			kStorageTexelBuffer = 2,
//			kUniformBuffer = 2,
//			kStorageBuffer = 2,

			// this MUST be manually set when reusing binding indices
			kNumOfBinds = 3
		};
	}

	VkMemoryPropertyFlags StorageTypeToVkMemoryPropertyFlags(StorageType storage) {
		VkMemoryPropertyFlags memFlags{0};
		switch (storage) {
			case StorageType::Device:
				memFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
				break;
			case StorageType::HostVisible: // dangerous
				memFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				break;
			case StorageType::Memoryless:
				memFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
				break;
		}
		return memFlags;
	}

	bool ValidateRange(const VkExtent3D& extent3D, uint32_t numLevels, const TexRange& range) {
		if (range.dimensions.width <= 0 ||
			range.dimensions.height <= 0 ||
			range.dimensions.depth <= 0 ||
			range.numLayers <= 0 ||
			range.numMipLevels <= 0) {
			LOG_USER(LogType::Error, "Values like: width, height, depth, numLayers and mipLevel must all be at least greater than 0.");
			return false;
		}
		if (range.mipLevel > numLevels) {
			LOG_USER(LogType::Error, "Requested mipLevels exceed texture's mipLevels!");
			return false;
		}
		const uint32_t texWidth = std::max(extent3D.width >> range.mipLevel, 1u);
		const uint32_t texHeight = std::max(extent3D.height >> range.mipLevel, 1u);
		const uint32_t texDepth = std::max(extent3D.depth >> range.mipLevel, 1u);

		if (range.dimensions.width > texWidth ||
			range.dimensions.height > texHeight ||
			range.dimensions.depth > texDepth) {
			LOG_USER(LogType::Error, "Range dimensions exceed texture dimensions!");
			return false;
		}
		if (range.offset.x > texWidth - range.dimensions.width ||
			range.offset.y > texHeight - range.dimensions.height ||
			range.offset.z > texDepth - range.dimensions.depth) {
			LOG_USER(LogType::Error, "Range dimensions exceed texture dimensions when accounting for offsets!");
			return false;
		}
		return true;
	}
	CTX::~CTX() {
		VK_CHECK(vkDeviceWaitIdle(_vkDevice));
		// more like awaitingDestruction :0
		_awaitingCreation = true;

		for (auto& plugin : _plugins) {
			plugin->onDispose();
		}

		_staging.reset(nullptr);
		_swapchain.reset(nullptr);
		vkDestroySemaphore(_vkDevice, _timelineSemaphore, nullptr);
		destroy(_dummyTextureHandle);
		destroy(_linearSamplerHandle);
		destroy(_nearestSamplerHandle);


		// TODO::fixing the allocation not being proerly freed for VMA
		if (_shaderPool.numObjects()) {
			LOG_USER(LogType::Info, "Cleaned up {} shader modules", _shaderPool.numObjects());
			for (int i = 0; i < _shaderPool._objects.size(); i++) {
				destroy(_shaderPool.getHandle(_shaderPool.findObject(&_shaderPool._objects[i]._obj).index()));
			}
		}
		if (_graphicsPipelinePool.numObjects()) {
			LOG_USER(LogType::Info, "Cleaned up {} render pipelines", _graphicsPipelinePool.numObjects());
			for (int i = 0; i < _graphicsPipelinePool._objects.size(); i++) {
				destroy(_graphicsPipelinePool.getHandle(_graphicsPipelinePool.findObject(&_graphicsPipelinePool._objects[i]._obj).index()));
			}
		}
		if (_samplerPool.numObjects() > 1) {
			// the dummy value is owned by the context
			LOG_USER(LogType::Info, "Cleaned up {} samplers", _samplerPool.numObjects() - 1);
			for (int i = 0; i < _samplerPool._objects.size(); i++) {
				destroy(_samplerPool.getHandle(_samplerPool.findObject(&_samplerPool._objects[i]._obj).index()));
			}
		}
		if (_texturePool.numObjects()) {
			LOG_USER(LogType::Info, "Cleaned up {} textures", _texturePool.numObjects());
			for (int i = 0; i < _texturePool._objects.size(); i++) {
				destroy(_texturePool.getHandle(_texturePool.findObject(&_texturePool._objects[i]._obj).index()));
			}
		}
		if (_bufferPool.numObjects()) {
			LOG_USER(LogType::Info, "Cleaned up {} buffers", _bufferPool.numObjects());
			for (int i = 0; i < _bufferPool._objects.size(); i++) {
				destroy(_bufferPool.getHandle(_bufferPool.findObject(&_bufferPool._objects[i]._obj).index()));
			}
		}

		// wipe buffers
		_bufferPool.clear();
		_texturePool.clear();
		_samplerPool.clear();
		_shaderPool.clear();
		_graphicsPipelinePool.clear();
		// make sure imm tasks are complete
		waitDeferredTasks();
		_imm.reset(nullptr);

		// destroy slang compiler
		_slangSession.detach();

		vkDestroyDescriptorSetLayout(_vkDevice, _vkDSL, nullptr);
		vkDestroyDescriptorPool(_vkDevice, _vkDPool, nullptr);

		// destroy our bindless descriptor stuff
		vkDestroyDescriptorSetLayout(_vkDevice, _vkBindlessDSL, nullptr);
		vkDestroyDescriptorPool(_vkDevice, _vkBindlessDPool, nullptr);

		vmaDestroyAllocator(_vmaAllocator);
		// destroy pure vk objects
		vkDestroyDevice(_vkDevice, nullptr);
#ifdef DEBUG
		vkDestroyDebugUtilsMessengerEXT(_vkInstance, _vkDebugMessenger, nullptr);
#endif
		SDL_Vulkan_DestroySurface(_vkInstance, _vkSurfaceKHR, nullptr);
		vkDestroyInstance(_vkInstance, nullptr);

		this->_window.destroy();
		SDL_Quit();
	}
	bool CTX::isSwapchainDirty() {
		return _swapchain->isDirty();
	}
	void CTX::cleanSwapchain() {
		if (_swapchain->isDirty()) {
			VkExtent2D res = this->_window.getFramebufferSize();
			LOG_USER(LogType::Info, "New Swapchain Extent: {} {}", res.width, res.height);

			VK_CHECK(vkDeviceWaitIdle(_vkDevice));
			_swapchain.reset(nullptr);
			vkDestroySemaphore(_vkDevice, _timelineSemaphore, nullptr);
			_swapchain = std::make_unique<Swapchain>(*this, res.width, res.height);
			_timelineSemaphore = vkutil::CreateTimelineSemaphore(_vkDevice, _swapchain->getNumOfSwapchainImages()-1);
		} else {
			LOG_USER(LogType::Warning, "Cleaning (resizing) of Swapchain called when Swapchain is not dirty, ignoring.");
		}
	}
	// called everytime we cmd.cmdBeginRendering()
	void CTX::checkAndUpdateDescriptorSets() {
		if (!_awaitingCreation) {
			return;
		}
		uint32_t newMaxTextures = _currentMaxTextureCount;
		uint32_t newMaxSamplers = _currentMaxSamplerCount;

		while (_texturePool._objects.size() > newMaxTextures) {
			newMaxTextures *= 2;
		}
		while (_samplerPool._objects.size() > newMaxSamplers) {
			newMaxSamplers *= 2;
		}
		if (newMaxTextures != _currentMaxTextureCount || newMaxSamplers != _currentMaxSamplerCount || _awaitingNewImmutableSamplers) {
			growDescriptorPool(newMaxTextures, newMaxSamplers);
		}

		// IMAGES //
		std::vector<VkDescriptorImageInfo> infoSampledImages;
		std::vector<VkDescriptorImageInfo> infoStorageImages;
		const uint32_t numObjects = _texturePool.numObjects();
		infoSampledImages.reserve(numObjects);
		infoStorageImages.reserve(numObjects);
		// enforce dummyImage created on init to be asssigned if texture is missing
		VkImageView dummyImageView = _texturePool._objects[0]._obj._vkImageView;

		for (const auto& obj : _texturePool._objects) {
			const AllocatedTexture& img = obj._obj;
			VkImageView view = obj._obj._vkImageView;
			VkImageView storageView = obj._obj._vkImageViewStorage ? obj._obj._vkImageViewStorage : view;
			// multisampled images cannot be directly accessed from shaders
			const bool isTextureAvailable = (img.getSampleCount() & VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_1_BIT;
			const bool isSampledImage = isTextureAvailable && img.isSampledImage();
			const bool isStorageImage = isTextureAvailable && img.isStorageImage();
			infoSampledImages.push_back(VkDescriptorImageInfo{
					.sampler = VK_NULL_HANDLE,
					.imageView = isSampledImage ? view : dummyImageView,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			});
			LOG_DEBUG("{} - {}", std::string_view(img._debugName), _texturePool.findObject(&obj._obj).index());
			ASSERT_MSG(infoSampledImages.back().imageView != VK_NULL_HANDLE, "Sampled VkImageView is VK_NULL_HANDLE!");
			infoStorageImages.push_back(VkDescriptorImageInfo{
					.sampler = VK_NULL_HANDLE,
					.imageView = isStorageImage ? storageView : dummyImageView,
					.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			});
		}

		// SAMPLERS //
		std::vector<VkDescriptorImageInfo> infoSamplers;
		infoSamplers.reserve(_samplerPool._objects.size());

		VkSampler linearSampler = _samplerPool._objects[0]._obj._vkSampler;

		for (const auto& obj : _samplerPool._objects) {
			infoSamplers.push_back({
				.sampler = obj._obj._vkSampler ? obj._obj._vkSampler : linearSampler,
				.imageView = VK_NULL_HANDLE,
				.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				});
		}

		// now write to descriptor set //
		VkWriteDescriptorSet write[kNumOfBindings] = {};
		uint32_t numWrites = 0;

		if (!infoSampledImages.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkDSet,
					.dstBinding = kTextureBinding,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoSampledImages.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					.pImageInfo = infoSampledImages.data(),
			};
		}

		if (!infoSamplers.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkDSet,
					.dstBinding = kSamplerBinding,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoSamplers.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
					.pImageInfo = infoSamplers.data(),
			};
		}

		if (!infoStorageImages.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkDSet,
					.dstBinding = kStorageImageBinding,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoStorageImages.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					.pImageInfo = infoStorageImages.data(),
			};
		}

		// update descriptor set
		if (numWrites) {
			_imm->wait(_imm->getLastSubmitHandle());
			vkUpdateDescriptorSets(_vkDevice, numWrites, write, 0, nullptr);
		}
		_awaitingCreation = false;
	}

	// FIXME yeah cause this is hardcoded
	void CTX::updateDescriptorSet(InternalBufferHandle handle, const char* paramName) {
		AllocatedBuffer* buffer1 = &_bufferPool._objects[3]._obj;
		VkDescriptorBufferInfo info = {
				.buffer = buffer1->_vkBuffer,
				.offset = 0,
				.range = buffer1->_bufferSize
		};
		AllocatedBuffer* buffer2 = &_bufferPool._objects[4]._obj;
		VkDescriptorBufferInfo info1 = {
				.buffer = buffer2->_vkBuffer,
				.offset = 0,
				.range = buffer2->_bufferSize
		};

		VkDescriptorSet dSet0 = _graphicsPipelinePool._objects[0]._obj._nonbindlessDescriptorSets[1];
		VkWriteDescriptorSet write[2];
		write[0] = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = dSet0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &info
		};
		VkDescriptorSet dSet1 = _graphicsPipelinePool._objects[0]._obj._nonbindlessDescriptorSets[0];
		write[1] = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = dSet1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &info1
		};
		vkUpdateDescriptorSets(_vkDevice, 2, write, 0, nullptr);
	}

	void CTX::bindDefaultBindlessDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout) {
		const std::array<VkDescriptorSet, 1> descriptor_sets = { _vkBindlessDSet };
		vkCmdBindDescriptorSets(cmd, bindPoint, pipelineLayout, 2, descriptor_sets.size(), descriptor_sets.data(), 0, nullptr);
	}
	void CTX::checkAndUpdateBindlessDescriptorSet() {
		if (!_awaitingCreation) {
			return;
		}
		uint32_t newMaxSamplers = _currentMaxSamplerCount;
		uint32_t newMaxTextures = _currentMaxTextureCount;

		// grow max counts to accomadate ALL possiblely cretade textures and samplers
		while (_texturePool._objects.size() > newMaxTextures) {
			newMaxTextures *= 2;
		}
		while (_samplerPool._objects.size() > newMaxSamplers) {
			newMaxSamplers *= 2;
		}
		if (newMaxTextures != _currentMaxTextureCount || newMaxSamplers != _currentMaxSamplerCount || _awaitingNewImmutableSamplers) {
			growBindlessDescriptorPool(newMaxSamplers, newMaxTextures);
		}
		// SAMPLERS //
		std::vector<VkDescriptorImageInfo> infoSamplers;
		infoSamplers.reserve(_samplerPool._objects.size());
		// our linear sampler is the backup/resolve
		VkSampler linearSampler = _samplerPool._objects[0]._obj._vkSampler;
		for (const auto& obj : _samplerPool._objects) {
			infoSamplers.push_back({
				.sampler = obj._obj._vkSampler ? obj._obj._vkSampler : linearSampler,
				.imageView = VK_NULL_HANDLE,
				.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED
			});
		}

		// IMAGES //
		std::vector<VkDescriptorImageInfo> infoSampledImages;
		std::vector<VkDescriptorImageInfo> infoStorageImages;
		const uint32_t numTextureObjects = _texturePool.numObjects();
		infoSampledImages.reserve(numTextureObjects);
		infoStorageImages.reserve(numTextureObjects);
		// our dummyTexture, the cross pattern, is the backup/resolve
		VkImageView dummyImageView = _texturePool._objects[0]._obj._vkImageView;
		for (const auto& obj : _texturePool._objects) {
			const AllocatedTexture& img = obj._obj;
			VkImageView view = obj._obj._vkImageView;
			VkImageView storageView = obj._obj._vkImageViewStorage ? obj._obj._vkImageViewStorage : view;
			// multisampled images cannot be directly accessed from shaders
			const bool isTextureAvailable = (img.getSampleCount() & VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_1_BIT;
			const bool isSampledImage = isTextureAvailable && img.isSampledImage();
			const bool isStorageImage = isTextureAvailable && img.isStorageImage();
			// sampled images have no either layout need but shader_read_only
			infoSampledImages.push_back(VkDescriptorImageInfo{
					.sampler = VK_NULL_HANDLE,
					.imageView = isSampledImage ? view : dummyImageView,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			});
			LOG_DEBUG("{} - {}", img._debugName, _texturePool.findObject(&obj._obj).index());
			ASSERT_MSG(infoSampledImages.back().imageView != VK_NULL_HANDLE, "Sampled imageView is null!");
			// storage images could be used for anything, per in general layout
			infoStorageImages.push_back(VkDescriptorImageInfo{
					.sampler = VK_NULL_HANDLE,
					.imageView = isStorageImage ? storageView : dummyImageView,
					.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			});
		}
		// BUFFERS //
		// currently we do not support bindless buffers, though this is pretty easy to implement
//		std::vector<VkDescriptorBufferInfo> infoUniformBuffers;
//		std::vector<VkDescriptorBufferInfo> infoStorageBuffers;
//		const uint32_t numBufferObjects = _bufferPool.numObjects();
//		for (const auto& obj : _bufferPool._objects) {
//			const AllocatedBuffer& buf = obj._obj;
//			// get vulkan objects we want
//			VkBuffer buffer = obj._obj._vkBuffer;
//			// get states we need to decide on how to add to vectors
//			const bool isUniformBuffer = buf.isUniformBuffer();
//			const bool isStorageBuffer = buf.isStorageBuffer();
//			infoUniformBuffers.push_back(VkDescriptorBufferInfo{
//				.buffer = buffer,
//				.offset = 0,
//				.range = isUniformBuffer ? buf._bufferSize : VK_WHOLE_SIZE
//			});
//			LOG_DEBUG("{} - {}", buf._debugName, _bufferPool.findObject(&obj._obj).index());
//			ASSERT_MSG(infoUniformBuffers.back().buffer != VK_NULL_HANDLE, "Uniform VkBuffer is VK_NULL_HANDLE!");
//			infoStorageBuffers.push_back(VkDescriptorBufferInfo{
//				.buffer = buffer,
//				.offset = 0,
//				.range = isStorageBuffer ? buf._bufferSize : VK_WHOLE_SIZE
//			});
//		}

		VkWriteDescriptorSet write[BindlessSpaceIndex::kNumOfBinds] = {};
		uint32_t numWrites = 0;
		if (!infoSamplers.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkBindlessDSet,
					.dstBinding = BindlessSpaceIndex::kSampler,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoSamplers.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
					.pImageInfo = infoSamplers.data(),
			};
		}

		// currently not supported as there is no way for the user to make a combined image+sampler
//		if (!infoSamplers.empty()) {
//			write[numWrites++] = VkWriteDescriptorSet{
//					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
//					.dstSet = _vkDSet,
//					.dstBinding = BindlessSpaceIndex::kCombinedImageSampler,
//					.dstArrayElement = 0,
//					.descriptorCount = static_cast<uint32_t>(infoSampledImages.size()),
//					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
//					.pImageInfo = infoSamplers.data(),
//			};
//		}

		if (!infoSampledImages.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkBindlessDSet,
					.dstBinding = BindlessSpaceIndex::kSampledImage,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoSampledImages.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					.pImageInfo = infoSampledImages.data(),
			};
		}
		if (!infoStorageImages.empty()) {
			write[numWrites++] = VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = _vkBindlessDSet,
					.dstBinding = BindlessSpaceIndex::kStorageImage,
					.dstArrayElement = 0,
					.descriptorCount = static_cast<uint32_t>(infoStorageImages.size()),
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					.pImageInfo = infoStorageImages.data(),
			};
		}
		if (numWrites) {
			_imm->wait(_imm->getLastSubmitHandle());
			vkUpdateDescriptorSets(_vkDevice, numWrites, write, 0, nullptr);
		}
		_awaitingCreation = false;
	}

	void CTX::growBindlessDescriptorPool(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount) {
		_currentMaxSamplerCount = newMaxSamplerCount;
		_currentMaxTextureCount = newMaxTextureCount;

		const uint32_t MAX_TEXTURE_LIMIT = _vkPhysDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages;
		ASSERT_MSG(newMaxTextureCount <= MAX_TEXTURE_LIMIT, "Max sampled textures exceeded: {}, but maximum of {} is allowed!", newMaxTextureCount, MAX_TEXTURE_LIMIT);

		const uint32_t MAX_SAMPLER_LIMIT = _vkPhysDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSamplers;
		ASSERT_MSG(newMaxSamplerCount <= MAX_SAMPLER_LIMIT, "Max samplers exceeded: {}, but maximum of {} is allowed!", newMaxSamplerCount, MAX_SAMPLER_LIMIT);

		if (_vkBindlessDSL != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dsl = _vkBindlessDSL]() {
				vkDestroyDescriptorSetLayout(device, dsl, nullptr);
			}));
		}
		if (_vkBindlessDPool != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dp = _vkBindlessDPool]() {
				vkDestroyDescriptorPool(device, dp, nullptr);
			}));
		}

		VkShaderStageFlags stage_flags =
				VK_SHADER_STAGE_VERTEX_BIT |
				VK_SHADER_STAGE_FRAGMENT_BIT |
				VK_SHADER_STAGE_COMPUTE_BIT;
		const VkDescriptorSetLayoutBinding bindings[BindlessSpaceIndex::kNumOfBinds] = {
				// __DynamicResource<__DynamicResourceKind.Sampler>
				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kSampler, VK_DESCRIPTOR_TYPE_SAMPLER, newMaxSamplerCount, stage_flags, nullptr),
				// __DynamicResource<__DynamicResourceKind.General>
//				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kCombinedImageSampler, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, newMaxTextureCount, stage_flags, nullptr),
				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kSampledImage, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, newMaxTextureCount, stage_flags, nullptr),
				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kStorageImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, newMaxTextureCount, stage_flags, nullptr),
//				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kUniformTexelBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, newMaxTextureCount, stage_flags, nullptr),
//				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kStorageTexelBuffer, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, newMaxTextureCount, stage_flags, nullptr),
//				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kUniformBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, newMaxTextureCount, stage_flags, nullptr),
//				VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kStorageBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, newMaxTextureCount, stage_flags, nullptr),
		};
		const uint32_t dsbinding_flags =
				VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
				VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
				VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		// assign each of our descriptors the same flags
		VkDescriptorBindingFlags bindingFlags[BindlessSpaceIndex::kNumOfBinds];
		for (VkDescriptorBindingFlags& bindingFlag : bindingFlags) {
			bindingFlag = dsbinding_flags;
		}
		const VkDescriptorSetLayoutBindingFlagsCreateInfo dsl_bf_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
				.bindingCount = (uint32_t) BindlessSpaceIndex::kNumOfBinds,
				.pBindingFlags = bindingFlags,
		};
		const VkDescriptorSetLayoutCreateInfo dsl_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.pNext = &dsl_bf_ci,
				.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
				.bindingCount = (uint32_t) BindlessSpaceIndex::kNumOfBinds,
				.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(_vkDevice, &dsl_ci, nullptr, &_vkBindlessDSL));

		const VkDescriptorPoolSize poolSizes[BindlessSpaceIndex::kNumOfBinds]{
			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_SAMPLER, newMaxSamplerCount),
//			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, newMaxTextureCount),
			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, newMaxTextureCount),
			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, newMaxTextureCount),
//			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, newMaxTextureCount),
//			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, newMaxTextureCount),
//			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, newMaxTextureCount),
//			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, newMaxTextureCount),
		};
		const VkDescriptorPoolCreateInfo dp_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
				.maxSets = 1,
				.poolSizeCount = BindlessSpaceIndex::kNumOfBinds,
				.pPoolSizes = poolSizes,
		};
		VK_CHECK(vkCreateDescriptorPool(_vkDevice, &dp_ci, nullptr, &_vkBindlessDPool));
		const VkDescriptorSetAllocateInfo ds_ai = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = _vkBindlessDPool,
				.descriptorSetCount = 1,
				.pSetLayouts = &_vkBindlessDSL,
		};
		VK_CHECK(vkAllocateDescriptorSets(_vkDevice, &ds_ai, &_vkBindlessDSet));
		_awaitingNewImmutableSamplers = false;
	}

	void CTX::growDescriptorPool(uint32_t newMaxTextureCount, uint16_t newMaxSamplerCount) {
		// update maxes
		_currentMaxTextureCount = newMaxTextureCount;
		_currentMaxSamplerCount = newMaxSamplerCount;

		const uint32_t MAX_TEXTURE_LIMIT = _vkPhysDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages;
		ASSERT_MSG(newMaxTextureCount <= MAX_TEXTURE_LIMIT, "Max sampled textures exceeded: {}, but maximum of {} is allowed!", newMaxTextureCount, MAX_TEXTURE_LIMIT);

		const uint32_t MAX_SAMPLER_LIMIT = _vkPhysDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSamplers;
		ASSERT_MSG(newMaxSamplerCount <= MAX_SAMPLER_LIMIT, "Max samplers exceeded: {}, but maximum of {} is allowed!", newMaxSamplerCount, MAX_SAMPLER_LIMIT);

		if (_vkDSL != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dsl = _vkDSL]() {
				vkDestroyDescriptorSetLayout(device, dsl, nullptr);
			}));
		}
		if (_vkDPool != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dp = _vkDPool]() {
				vkDestroyDescriptorPool(device, dp, nullptr);
			}));
		}

		VkShaderStageFlags stage_flags =
				VK_SHADER_STAGE_VERTEX_BIT |
				VK_SHADER_STAGE_FRAGMENT_BIT |
				VK_SHADER_STAGE_COMPUTE_BIT;

		const VkDescriptorSetLayoutBinding bindings[kNumOfBindings] = {
				VkDescriptorSetLayoutBinding(kTextureBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, newMaxTextureCount, stage_flags, nullptr),
				VkDescriptorSetLayoutBinding(kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, newMaxSamplerCount, stage_flags, nullptr),
				VkDescriptorSetLayoutBinding(kStorageImageBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, newMaxTextureCount, stage_flags, nullptr),
		};
		// setup for set layout, all this extra stuff is just for our bindless sytem to work
		const uint32_t dsbinding_flags =
				VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
				VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
				VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		// assign each of our descriptors the same flags
		VkDescriptorBindingFlags bindingFlags[kNumOfBindings];
		for (VkDescriptorBindingFlags& bindingFlag : bindingFlags) {
			bindingFlag = dsbinding_flags;
		}
		const VkDescriptorSetLayoutBindingFlagsCreateInfo dsl_bf_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
				.bindingCount = (uint32_t) kNumOfBindings,
				.pBindingFlags = bindingFlags,
		};
		const VkDescriptorSetLayoutCreateInfo dsl_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.pNext = &dsl_bf_ci,
				.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
				.bindingCount = (uint32_t) kNumOfBindings,
				.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(_vkDevice, &dsl_ci, nullptr, &_vkDSL));

		// now create the descriptor sets according to the just created layout

		const VkDescriptorPoolSize poolSizes[kNumOfBindings]{
				VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, newMaxTextureCount},
				VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, newMaxSamplerCount},
				VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, newMaxTextureCount},
		};
		const VkDescriptorPoolCreateInfo dp_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
				.maxSets = 1,
				.poolSizeCount = (uint32_t)kNumOfBindings,
				.pPoolSizes = poolSizes,
		};
		VK_CHECK(vkCreateDescriptorPool(_vkDevice, &dp_ci, nullptr, &_vkDPool));
		const VkDescriptorSetAllocateInfo ds_ai = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = _vkDPool,
				.descriptorSetCount = 1,
				.pSetLayouts = &_vkDSL,
		};
		VK_CHECK(vkAllocateDescriptorSets(_vkDevice, &ds_ai, &_vkDSet));

		_awaitingNewImmutableSamplers = false;
	}
	void CTX::bindDefaultDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) {
		const std::array<VkDescriptorSet, 1> descriptor_sets = { _vkDSet };
		vkCmdBindDescriptorSets(cmd, bindPoint, layout, 0, descriptor_sets.size(), descriptor_sets.data(), 0, nullptr);
	}
	void CTX::deferTask(std::packaged_task<void()>&& task, SubmitHandle handle) const {
		if (handle.empty()) {
			handle = _imm->getNextSubmitHandle();
		}
		_deferredTasks.emplace_back(std::move(task), handle);
	}
	void CTX::processDeferredTasks() {
		auto it = _deferredTasks.begin();
		while (it != _deferredTasks.end() && _imm->isReady(it->_handle, true)) {
			(it++)->_task();
		}
		_deferredTasks.erase(_deferredTasks.begin(), it);
	}
	void CTX::waitDeferredTasks() {
		for (auto& task : _deferredTasks) {
			_imm->wait(task._handle);
			task._task();
		}
		_deferredTasks.clear();
	}
	VkDeviceAddress CTX::gpuAddress(InternalBufferHandle handle, size_t offset) {
		const AllocatedBuffer* buf = _bufferPool.get(handle);
		ASSERT_MSG(buf && buf->_vkDeviceAddress, "Buffer doesnt have a valid device address!");
		return buf->_vkDeviceAddress + offset;
	}
	GraphicsPipeline* CTX::resolveRenderPipeline(InternalPipelineHandle handle) {
		GraphicsPipeline* graphics_pipeline = _graphicsPipelinePool.get(handle);
		if (!graphics_pipeline) {
			LOG_USER(LogType::Warning, "Graphics pipeline does not exist, use a valid handle!");
			return VK_NULL_HANDLE;
		}
		// updating descriptor layout //
		if (graphics_pipeline->_vkLastDescriptorSetLayout != _vkBindlessDSL) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = graphics_pipeline->_vkPipeline]() {
				vkDestroyPipeline(device, pipeline, nullptr);
			}));
			deferTask(std::packaged_task<void()>([device = _vkDevice, layout = graphics_pipeline->_vkPipelineLayout]() {
				vkDestroyPipelineLayout(device, layout, nullptr);
			}));
			graphics_pipeline->_vkPipeline = VK_NULL_HANDLE;
			graphics_pipeline->_vkLastDescriptorSetLayout = _vkBindlessDSL;
		}

		// RETURN EXISTING PIPELINE //
		if (graphics_pipeline->_vkPipeline != VK_NULL_HANDLE) {
			return graphics_pipeline;
		}
		// or, CREATE NEW PIPELINE //

		PipelineSpec& spec = graphics_pipeline->_spec;

		// things the user defined previously
		PipelineBuilder builder = {};
		builder.set_cull_mode(spec.cull);
		builder.set_polygon_mode(spec.polygon);
		builder.set_topology_mode(spec.topology);
		builder.set_multisampling_mode(spec.multisample);
		builder.set_blending_mode(spec.blend);

		// things we gather in the middle of the renderpass (attachment formats)
		// looks at the current pass and reflects those formats in use
		const PassCompiled& current_pass_info = _currentCommandBuffer._activePass;
		std::vector<VkFormat> colorFormats;
		colorFormats.reserve(current_pass_info.colorAttachments.size());
		for (auto& colorAttachment : current_pass_info.colorAttachments) {
			colorFormats.push_back(colorAttachment.imageFormat);
		}
		builder.set_color_formats(colorFormats);
		if (current_pass_info.depthAttachment.has_value()) {
			builder.set_depth_format(current_pass_info.depthAttachment->imageFormat);
		}

		// shader setup
		for (const PipelineSpec::ShaderStage& stage : spec.stages) {
			builder.add_shader_module(_shaderPool.get(stage.handle)->vkShaderModule, toVulkan(stage.stage), stage.entryPoint);
		}

		auto shader = _shaderPool.get(spec.stages.front().handle);

		VkPushConstantRange range = {
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = 128,
		};
		// system sets first, bindless setindex = 0, everything else 1+
		const std::array<VkDescriptorSetLayout, 1> systemSetLayouts = { _vkBindlessDSL };


		std::vector<VkDescriptorSetLayout> totalSetLayouts = shader->_vkDescriptorSetLayouts;

		for (auto& totalSetLayout : totalSetLayouts) {
			const VkDescriptorPoolSize poolSizes[1] = {
					VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1)
			};
			const VkDescriptorPoolCreateInfo dp_ci = {
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
					.flags = 0,
					.maxSets = 1,
					.poolSizeCount = 1,
					.pPoolSizes = poolSizes,
			};
			VkDescriptorPool pool;
			VK_CHECK(vkCreateDescriptorPool(_vkDevice, &dp_ci, nullptr, &pool));
			const VkDescriptorSetAllocateInfo ds_ai = {
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					.descriptorPool = pool,
					.descriptorSetCount = 1,
					.pSetLayouts = &totalSetLayout,
			};
			VkDescriptorSet set;
			VK_CHECK(vkAllocateDescriptorSets(_vkDevice, &ds_ai, &set));
			graphics_pipeline->_nonbindlessDescriptorSets.push_back(set);
		}

//		uint32_t numSets = graphics_pipeline->_nonbindlessDescriptorSets.size();
//		VkWriteDescriptorSet write[numSets];
//		uint32_t writesN;
//		for (int i = 1; i < numSets; i++) {
//			write[writesN++] = VkWriteDescriptorSet{
//				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
//				.dstSet = i,
//				.dstBinding = 0,
//				.descriptorCount = 1,
//				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
//				.pBufferInfo =
//			};
//		}
//		_imm->wait(_imm->getLastSubmitHandle());
//		vkUpdateDescriptorSets(_vkDevice, writesN, write, 0, nullptr);

//		totalSetLayouts.insert(totalSetLayouts.end(), systemSetLayouts.begin(), systemSetLayouts.end());
		totalSetLayouts = { totalSetLayouts[0], totalSetLayouts[1], _vkBindlessDSL };
		const VkPipelineLayoutCreateInfo pipeline_layout_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = static_cast<uint32_t>(totalSetLayouts.size()),
				.pSetLayouts = totalSetLayouts.data(),
				.pushConstantRangeCount = 1u,
				.pPushConstantRanges =  &range,
		};
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VK_CHECK(vkCreatePipelineLayout(_vkDevice, &pipeline_layout_info, nullptr, &pipelineLayout));

		graphics_pipeline->_vkPipeline = builder.build(_vkDevice, pipelineLayout);
		graphics_pipeline->_vkPipelineLayout = pipelineLayout;

		return graphics_pipeline;
	}

	void CTX::generateMipmaps(InternalTextureHandle handle) {
		if (handle.empty()) {
			LOG_USER(LogType::Warning, "Generate mipmap request with empty handle!");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		if (image->_numLevels <= 1) {
			return;
		}
		ASSERT(image->_vkCurrentImageLayout != VK_IMAGE_LAYOUT_UNDEFINED);
		const ImmediateCommands::CommandBufferWrapper& wrapper = _imm->acquire();
		image->generateMipmap(wrapper._cmdBuf);
		_imm->submit(wrapper);
	}
	InternalSamplerHandle CTX::createSampler(SamplerSpec spec) {
		VkFilter minfilter = toVulkan(spec.minFilter);
		VkFilter magfilter = toVulkan(spec.magFilter);
		VkSamplerAddressMode addressU = toVulkan(spec.wrapU);
		VkSamplerAddressMode addressV = toVulkan(spec.wrapV);
		VkSamplerAddressMode addressW = toVulkan(spec.wrapW);

		// creating sampler requires little work so we dont need an _Impl function for it
		VkSamplerCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.pNext = nullptr,

				.magFilter = magfilter,
				.minFilter = minfilter,
				.addressModeU = addressU,
				.addressModeV = addressV,
				.addressModeW = addressW,

				.maxAnisotropy = 1.0f,
				.minLod = -1000,
				.maxLod = 1000,

				.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
				.unnormalizedCoordinates = false,
		};

		AllocatedSampler obj = {};
		vkCreateSampler(_vkDevice, &info, nullptr, &obj._vkSampler);
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);
		obj._debugName[sizeof(obj._debugName) - 1] = '\0';
		InternalSamplerHandle handle = _samplerPool.create(std::move(obj));
		_awaitingCreation = true;
		return {handle};
	}
	InternalBufferHandle CTX::createBuffer(BufferSpec spec) {
		VkBufferUsageFlags usage_flags = (spec.storage == StorageType::Device) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0;

		if (spec.usage & BufferUsageBits::BufferUsageBits_Index)
			usage_flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_Uniform)
			usage_flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_Storage)
			usage_flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_Indirect)
			usage_flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		ASSERT_MSG(usage_flags, "Invalid buffer creation specification!");
		const VkMemoryPropertyFlags mem_flags = StorageTypeToVkMemoryPropertyFlags(spec.storage);

		AllocatedBuffer obj = this->createBufferImpl(spec.size, usage_flags, mem_flags);
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);
		obj._debugName[sizeof(obj._debugName) - 1] = '\0';
		vmaSetAllocationName(_vmaAllocator, obj._vmaAllocation, obj._debugName);
		InternalBufferHandle handle = _bufferPool.create(std::move(obj));
		if (spec.data) {
			upload(handle, spec.data, spec.size);
		}
		_awaitingCreation = true;
		return {handle};
	}
	AllocatedBuffer CTX::createBufferImpl(VkDeviceSize bufferSize, VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memFlags) {
		ASSERT_MSG(bufferSize > 0, "Buffer size needs to be greater than 0!");

		AllocatedBuffer buf = {};
		buf._bufferSize = bufferSize;
		buf._vkUsageFlags = usageFlags;
		buf._vkMemoryPropertyFlags = memFlags;

		const VkBufferCreateInfo buffer_ci = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.size = bufferSize,
				.usage = usageFlags,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.queueFamilyIndexCount = 0,
				.pQueueFamilyIndices = nullptr,
		};
		VmaAllocationCreateInfo vmaAllocInfo = {};

		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			vmaAllocInfo = {
					.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
					.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
					.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			};
		}
		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			// Check if coherent buffer is available.
			VK_CHECK(vkCreateBuffer(_vkDevice, &buffer_ci, nullptr, &buf._vkBuffer));
			VkMemoryRequirements requirements = {};
			vkGetBufferMemoryRequirements(_vkDevice, buf._vkBuffer, &requirements);
			vkDestroyBuffer(_vkDevice, buf._vkBuffer, nullptr);
			buf._vkBuffer = VK_NULL_HANDLE;

			if (requirements.memoryTypeBits & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
				vmaAllocInfo.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				buf._isCoherentMemory = true;
			}
		}
		vmaAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		vmaCreateBufferWithAlignment(_vmaAllocator, &buffer_ci, &vmaAllocInfo, 16, &buf._vkBuffer, &buf._vmaAllocation, nullptr);
		// handle memory-mapped buffers
		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			vmaMapMemory(_vmaAllocator, buf._vmaAllocation, &buf._mappedPtr);
		}

		ASSERT_MSG(buf._vkBuffer != VK_NULL_HANDLE, "VkBuffer is VK_NULL_HANDLE after creation!");

		// shader access
		if (usageFlags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
			const VkBufferDeviceAddressInfo buffer_device_ai = {
					.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
					.buffer = buf._vkBuffer,
			};
			buf._vkDeviceAddress = vkGetBufferDeviceAddress(_vkDevice, &buffer_device_ai);
			ASSERT(buf._vkDeviceAddress);
		}
		return buf;
	}

	void CTX::resizeTexture(InternalTextureHandle handle, VkExtent2D newExtent) {
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image) return;

		// copies destroy for Texture logic
		deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageView]() {
			vkDestroyImageView(device, imageView, nullptr);
		}));
		if (image->_vkImageViewStorage) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageViewStorage]() {
				vkDestroyImageView(device, imageView, nullptr);
			}));
		}
		if (image->_isOwning) {
			if (image->_mappedPtr) {
				vmaUnmapMemory(_vmaAllocator, image->_vmaAllocation);
			}
			deferTask(std::packaged_task<void()>([vma = _vmaAllocator, image = image->_vkImage, allocation = image->_vmaAllocation]() {
				vmaDestroyImage(vma, image, allocation);
			}));
		}

		VkImageCreateFlags createFlags = 0;
		// resolve createFlags without having it stored
		if (image->_vkImageViewType == VK_IMAGE_VIEW_TYPE_CUBE ||
		image->_vkImageViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
			createFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		}

		VkExtent3D extent3D = { newExtent.width, newExtent.height, 1 };

		AllocatedTexture newImage = this->createTextureImpl(
				image->_vkUsageFlags,
				image->_vkMemoryPropertyFlags,
				extent3D,
				image->_vkFormat,
				image->_vkImageType,
				image->_vkImageViewType,
				image->_numLevels,
				image->_numLayers,
				image->_vkSampleCountFlagBits,
				createFlags
				);
		vmaSetAllocationName(_vmaAllocator, newImage._vmaAllocation, newImage._debugName);

		// update extent!
		image->_vkExtent = extent3D;

		// new opaque objects
		image->_vkImage = newImage._vkImage;
		image->_vkImageView = newImage._vkImageView;
		image->_vkImageViewStorage = newImage._vkImageViewStorage;
		image->_vmaAllocation = newImage._vmaAllocation;
		// reset of state
		image->_vkCurrentImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		image->_mappedPtr = newImage._mappedPtr;
	}
	InternalTextureHandle CTX::createTexture(TextureSpec spec) {
		ASSERT(spec.usage);
		// resolve usage flags
		VkImageUsageFlags usage_flags = (spec.storage == StorageType::Device) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;

		if (spec.usage & TextureUsageBits::TextureUsageBits_Sampled) {
			usage_flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (spec.usage & TextureUsageBits::TextureUsageBits_Storage) {
			ASSERT_MSG(spec.samples == SampleCount::X1, "Storage images cannot be multisampled!");
			usage_flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (spec.usage & TextureUsageBits::TextureUsageBits_Attachment) {
			usage_flags |= (vkutil::IsFormatDepthOrStencil(spec.format)) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (spec.storage == StorageType::Memoryless) {
				usage_flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
			}
		}
		if (spec.storage != StorageType::Memoryless) {
			// for now, always set this flag so we can read it back
			usage_flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (spec.numMipLevels == 0) {
			LOG_USER(LogType::Warning, "The number of mip levels specified must be greater than 0!");
			spec.numMipLevels = 1;
		}
		ASSERT_MSG(usage_flags != 0, "Invalid usage flags for texture creation!");
		ASSERT_MSG(spec.type == TextureType::Type_2D || spec.type == TextureType::Type_3D || spec.type == TextureType::Type_Cube, "Only 2D, 3D and Cube textures are supported.");

		// resolve sample count
		const VkSampleCountFlagBits sample_bits = toVulkan(spec.samples);
		// resolve memory flags
		const VkMemoryPropertyFlags mem_flags = StorageTypeToVkMemoryPropertyFlags(spec.storage);
		// resolve extent3D

		VkExtent3D extent3D = { spec.dimension.width, spec.dimension.height, 1};

		// resolve actions based on texture type
		uint32_t _numLayers = spec.numLayers;
		VkImageType _imagetype;
		VkImageViewType _imageviewtype;
		VkImageCreateFlags _imageCreateFlags = 0;
		switch (spec.type) {
			case TextureType::Type_2D:
				_imagetype = VK_IMAGE_TYPE_2D;
				_imageviewtype = (_numLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
				break;
			case TextureType::Type_3D:
				_imagetype = VK_IMAGE_TYPE_3D;
				_imageviewtype = VK_IMAGE_VIEW_TYPE_3D;
				break;
			case TextureType::Type_Cube:
				_imagetype = VK_IMAGE_TYPE_2D;
				_imageviewtype = (_numLayers > 1) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
				_numLayers *= 6;
				_imageCreateFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
				break;
			default:
				ASSERT_MSG(false, "Program should never reach this!");
		}

		AllocatedTexture obj = createTextureImpl(usage_flags, mem_flags, extent3D, spec.format, _imagetype, _imageviewtype, 1, _numLayers, sample_bits, _imageCreateFlags);
		// members that are only needed for recreation
		obj._vkMemoryPropertyFlags = mem_flags;
		obj._vkImageViewType = _imageviewtype;
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);
		vmaSetAllocationName(_vmaAllocator, obj._vmaAllocation, obj._debugName);
		InternalTextureHandle handle = _texturePool.create(std::move(obj));
		// if we have some data we want to upload, do that
		_awaitingCreation = true;
		if (spec.data) {
			ASSERT(spec.dataNumMipLevels <= spec.numMipLevels);
			ASSERT(spec.type == TextureType::Type_2D || spec.type == TextureType::Type_Cube);
			TexRange range = {
					.dimensions = extent3D,
					.numLayers = static_cast<uint32_t>((spec.type == TextureType::Type_Cube) ? 6 : 1),
					.numMipLevels = spec.dataNumMipLevels
			};
			this->upload(handle, spec.data, range);
			if (spec.generateMipmaps) {
				this->generateMipmaps(handle);
			}
		}
		return {handle};
	}
	AllocatedTexture CTX::createTextureImpl(VkImageUsageFlags usageFlags,
											VkMemoryPropertyFlags memFlags,
											VkExtent3D extent3D,
											VkFormat format,
											VkImageType imageType,
											VkImageViewType imageViewType,
											uint32_t numLevels,
											uint32_t numLayers,
											VkSampleCountFlagBits sampleCountFlagBits,
											VkImageCreateFlags createFlags) {
		ASSERT_MSG(numLevels > 0, "The texture must contain at least one mip-level!");
		ASSERT_MSG(numLayers > 0, "The texture must contain at least one layer!");
		ASSERT_MSG(extent3D.width > 0, "The texture must have a width greater than 0!");
		ASSERT_MSG(extent3D.height > 0, "The texture must have a height greater than 0!");
		ASSERT_MSG(extent3D.depth > 0, "The texture must have a depth greater than 0!");

		VkImageCreateFlags image_cf = createFlags;

		VkImageCreateInfo image_ci = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.pNext = nullptr,

				.flags = image_cf,
				.imageType = imageType,
				.format = format,
				.extent = extent3D,
				.mipLevels = numLevels,
				.arrayLayers = numLayers,
				.samples = sampleCountFlagBits,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = usageFlags,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,

				.queueFamilyIndexCount = 0,
				.pQueueFamilyIndices = nullptr,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		// initialize before external memory, as some flags need to be set for VMA
		VmaAllocationCreateInfo allocation_ci = {};
		allocation_ci.usage = (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_AUTO;

		AllocatedTexture obj = {};
		VK_CHECK(vmaCreateImage(_vmaAllocator, &image_ci, &allocation_ci, &obj._vkImage, &obj._vmaAllocation, nullptr));
		obj._vkExtent = extent3D;
		obj._vkUsageFlags = usageFlags;
		obj._vkSampleCountFlagBits = sampleCountFlagBits;
		obj._vkImageType = imageType;
		obj._vkFormat = format;
		obj._numLayers = numLayers;
		obj._numLevels = numLevels;
		vkGetPhysicalDeviceFormatProperties(_vkPhysicalDevice, obj._vkFormat, &obj._vkFormatProperties);

		// if memory is manually managed on host
		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			vmaMapMemory(_vmaAllocator, obj._vmaAllocation, &obj._mappedPtr);
		}
		// create image views
		const VkImageAspectFlags aspectMask = vkutil::AspectMaskFromFormat(obj._vkFormat);
		const VkImageViewCreateInfo image_view_ci = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.pNext = nullptr,
				.image = obj._vkImage,
				.viewType = imageViewType,
				.format = format,
				.subresourceRange = {
						.aspectMask = aspectMask,
						.baseMipLevel = 0,
						.levelCount = VK_REMAINING_MIP_LEVELS,
						.baseArrayLayer = 0,
						.layerCount = numLayers
				}
		};
		VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &obj._vkImageView));
		if (obj._vkUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) {
			VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &obj._vkImageViewStorage));
		}
		return obj;
	}
	InternalPipelineHandle CTX::createPipeline(PipelineSpec spec) {
		// all creating a pipeline does is assign the spec to it
		// actual construction is done upon first use (lazily)
		GraphicsPipeline pipeline = {};
		pipeline._spec = std::move(spec);
		InternalPipelineHandle handle = _graphicsPipelinePool.create(std::move(pipeline));
		return handle;
	}

	Slang::ComPtr<slang::ISession> CTX::createSlangSession() {
		Slang::ComPtr<slang::IGlobalSession> globalSession;
		SlangResult res = slang::createGlobalSession(globalSession.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(res), "Slang failed to create global session!");

		slang::TargetDesc targetDesc = {
				.format = SLANG_SPIRV,
				.profile = globalSession->findProfile("spirv_1_5")
		};
		// by default emits spirv
		std::array<slang::CompilerOptionEntry, 5> entries = {
				slang::CompilerOptionEntry{
						.name = slang::CompilerOptionName::VulkanUseEntryPointName,
						.value = {
								.kind = slang::CompilerOptionValueKind::Int,
								.intValue0 = true
						}
				},
				slang::CompilerOptionEntry{
						.name = slang::CompilerOptionName::Optimization,
						.value = {
								.kind = slang::CompilerOptionValueKind::Int,
								.intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT
						}
				},
				slang::CompilerOptionEntry{
						.name = slang::CompilerOptionName::VulkanInvertY,
						.value = {
								.kind = slang::CompilerOptionValueKind::Int,
								.intValue0 = true
						}
				},
				slang::CompilerOptionEntry{
						.name = slang::CompilerOptionName::Capability,
						.value = {
								.kind = slang::CompilerOptionValueKind::String,
								.stringValue0 = "vk_mem_model"
						}
				},
				slang::CompilerOptionEntry{
					.name = slang::CompilerOptionName::BindlessSpaceIndex,
					.value = {
							.kind = slang::CompilerOptionValueKind::Int,
							.intValue0 = 0,
					}
				}
		};

		// TODO: perhaps find a more interesting way of doing this
		std::vector<const char*> ptrs;
		ptrs.reserve(_shaderSearchPaths.size());
		for (auto& s : _shaderSearchPaths)
			ptrs.push_back(s.c_str());

		slang::SessionDesc sessionDesc = {
				.targets = &targetDesc,
				.targetCount = 1,

				.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,

				.searchPaths = ptrs.data(),
				.searchPathCount = static_cast<int64_t>(ptrs.size()),
				.compilerOptionEntries = entries.data(),
				.compilerOptionEntryCount = entries.size(),
		};
		Slang::ComPtr<slang::ISession> session;
		globalSession->createSession(sessionDesc, session.writeRef());
		return session;
	}
	void compileSlangModule(const Slang::ComPtr<slang::ISession>& session, const char* filepath, slang::IBlob** spirvBlob, slang::ShaderReflection** reflection) {
		// 1. load module
		Slang::ComPtr<slang::IModule> module;
		Slang::ComPtr<slang::IBlob> diagnosticsBlob;
		module = session->loadModule(filepath, diagnosticsBlob.writeRef());
		ASSERT_MSG(module, "Failed Slang module creation! Diagnostics Below:\n{}", static_cast<const char*>(diagnosticsBlob->getBufferPointer()));
		diagnosticsBlob.setNull();

		// 2. compose module
		std::array<slang::IComponentType *, 1> componentTypes = { module };
		Slang::ComPtr<slang::IComponentType> composedProgram;
		SlangResult module_result = session->createCompositeComponentType(componentTypes.data(), componentTypes.size(),
																		  composedProgram.writeRef(),
																		  diagnosticsBlob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(module_result), "Composition failed! Diagnostics Below:\n{}", static_cast<const char*>(diagnosticsBlob->getBufferPointer()));
		diagnosticsBlob.setNull();

		// 2.5. retrieve layout for reflection
		// will always be 0 for raster shaders
		int targetIndex = 0;
		slang::ProgramLayout* progLayout = composedProgram->getLayout(targetIndex, diagnosticsBlob.writeRef());
		ASSERT_MSG(progLayout, "Failed to get ProgramLayout*, therefore shader reflection will also fail!");
		*reflection = progLayout;
		diagnosticsBlob.setNull();

		// 3. linking
		Slang::ComPtr<slang::IComponentType> linkedProgram;
		SlangResult compose_result = composedProgram->link(linkedProgram.writeRef(), diagnosticsBlob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(compose_result), "Linking failed! Diagnostics Below:\n{}", static_cast<const char*>(diagnosticsBlob->getBufferPointer()));
		diagnosticsBlob.setNull();

		// 4. retrieve kernel code
		SlangResult code_result = linkedProgram->getTargetCode(0, spirvBlob, diagnosticsBlob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(code_result), "Code retrieval failed! Diagnostics Below:\n{}", static_cast<const char*>(diagnosticsBlob->getBufferPointer()));
	}

	InternalShaderHandle CTX::createShader(ShaderSpec spec) {
		// lazily create slang session & slang global session
		if (!_slangSession) {
			_slangSession = createSlangSession();
		}
		Slang::ComPtr<slang::IBlob> spirvBlob;
		// reflection is guaranteed
		slang::ShaderReflection* reflection;
		// currently we do not reflect entry points
		compileSlangModule(_slangSession, spec.filePath, spirvBlob.writeRef(), &reflection);
		// how we need to always begin reflection for grabbing paramaters
		Shader obj;
		// everything else not below
		// TODO: this is some of the worst code i have ever written in my life, i am so sorry future me who will come back here and have to clean it
		ShaderTransformer transformer;
		transformer.performReflection(obj, reflection);
		VkPipelineLayout pipelineLayout = transformer.retrievePipelineLayout(_vkDevice);
		std::vector<VkDescriptorSetLayout> dsl_vector = transformer.retrieveDescriptorSetLayouts();

		// vulkan objects
		obj._vkDescriptorSetLayouts = std::move(dsl_vector);
		obj.vkPipelineLayout = pipelineLayout;

		// _vkShaderModule
		VkShaderModuleCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pNext = nullptr };
		create_info.flags = 0;
		create_info.pCode = static_cast<uint32_t const*>(spirvBlob->getBufferPointer());
		create_info.codeSize = spirvBlob->getBufferSize();
		VkShaderModule shaderModule;
		VK_CHECK(vkCreateShaderModule(_vkDevice, &create_info, nullptr, &shaderModule));
		obj.vkShaderModule = shaderModule;

		// _debugName
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);

		return _shaderPool.create(std::move(obj));
	}

	void CTX::upload(InternalBufferHandle handle, const void* data, size_t size, size_t offset) {
		if (!data) {
			LOG_USER(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		ASSERT_MSG(size > 0, "Size must be greater than 0!");

		AllocatedBuffer* buffer = _bufferPool.get(handle);

		if (offset + size > buffer->_bufferSize) {
			LOG_USER(LogType::Error, "Buffer request to upload is out of range! (Either the uploaded data size exceeds the size of the actual buffer or its offset is exceeding the total range)");
			return;
		}
		_staging->bufferSubData(*buffer, offset, size, data);
	}
	void CTX::download(InternalBufferHandle handle, void* data, size_t size, size_t offset) {
		if (!data) {
			LOG_USER(LogType::Warning, "Data is null");
			return;
		}
		AllocatedBuffer* buffer = _bufferPool.get(handle);

		if (!buffer) {
			LOG_USER(LogType::Error, "Retrieved buffer is null, handle must have been invalid!");
			return;
		}
		if (offset + size <= buffer->_bufferSize) {
			LOG_USER(LogType::Error, "Buffer request to download is out of range!");
			return;
		}
		buffer->getBufferSubData(*this, offset, size, data);
	}
	void CTX::upload(InternalTextureHandle handle, const void* data, const TexRange& range) {
		if (!data) {
			LOG_USER(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		ASSERT_MSG(image, "Attempting to use texture via invalid handle!");
		if (!ValidateRange(image->_vkExtent, image->_numLevels, range)) {
			LOG_USER(LogType::Warning, "Image failed validation check!");
		}
		// why is this here
		if (image->_vkImageType == VK_IMAGE_TYPE_3D) {
			_staging->imageData3D(
					*image,
					VkOffset3D{range.offset.x, range.offset.y, range.offset.z},
					VkExtent3D{range.dimensions.width, range.dimensions.height, range.dimensions.depth},
					image->_vkFormat,
					data);
		} else {
			const VkRect2D image_region = {
					.offset = {.x = range.offset.x, .y = range.offset.y},
					.extent = {.width = range.dimensions.width, .height = range.dimensions.height},
			};
			_staging->imageData2D(*image, image_region, range.mipLevel, range.numMipLevels, range.layer, range.numLayers, image->_vkFormat, data);
		}
	}
	void CTX::download(InternalTextureHandle handle, void* data, const TexRange &range) {
		if (!data) {
			LOG_USER(LogType::Warning, "Data is null.");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image) {
			LOG_USER(LogType::Error, "Retrieved image is null, handle must have been invalid!");
			return;
		}
		if (!ValidateRange(image->_vkExtent, image->_numLevels, range)) {
			LOG_USER(LogType::Warning, "Image validation failed!");
			return;
		}
		_staging->getImageData(*image,
							   VkOffset3D{range.offset.x, range.offset.y, range.offset.z},
							   VkExtent3D{range.dimensions.width, range.dimensions.height, range.dimensions.depth},
							   VkImageSubresourceRange{
									   .aspectMask = vkutil::AspectMaskFromFormat(image->_vkFormat),
									   .baseMipLevel = range.mipLevel,
									   .levelCount = range.numMipLevels,
									   .baseArrayLayer = range.layer,
									   .layerCount = range.numLayers,
							   },
							   image->_vkFormat,
							   data);
	}

	CommandBuffer& CTX::openCommand(CommandBuffer::Type type) {
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot open more than 1 CommandBuffer simultaneously!");
		_currentCommandBuffer = CommandBuffer(this, type);
		if (type == CommandBuffer::Type::Graphics) {
			_swapchain->acquire();
		}
		return _currentCommandBuffer;
	}
	SubmitHandle CTX::submitCommand(CommandBuffer& cmd) {
		ASSERT(cmd._ctx);
		ASSERT(cmd._wrapper);
		const bool isPresenting = cmd._cmdType == CommandBuffer::Type::Graphics;
		if (isPresenting) {
			ASSERT_MSG(_swapchain->_vkCurrentImageLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					   "Swapchain image layout is not VK_IMAGE_LAYOUT_PRESENT_SRC_KHR!");

			const uint64_t signalValue = _swapchain->_currentFrameNum + _swapchain->getNumOfSwapchainImages();
			_swapchain->_timelineWaitValues[_swapchain->_currentImageIndex] = signalValue;
			_imm->signalSemaphore(_timelineSemaphore, signalValue);
		}

		cmd._lastSubmitHandle = _imm->submit(*cmd._wrapper);
		if (isPresenting) {
			_swapchain->present();
		}
		processDeferredTasks();
		SubmitHandle handle = cmd._lastSubmitHandle;
		// reset
		_currentCommandBuffer = {};
		// might return the submit handle no clue
		return handle;
	}

	void CTX::destroy(InternalBufferHandle handle) {
		AllocatedBuffer* buf = _bufferPool.get(handle);
		if (!buf) {
			return;
		}
		if (buf->_mappedPtr) {
			vmaUnmapMemory(_vmaAllocator, buf->_vmaAllocation);
		}
		deferTask(std::packaged_task<void()>([vma = _vmaAllocator, buffer = buf->_vkBuffer, allocation = buf->_vmaAllocation]() {
			vmaDestroyBuffer(vma, buffer, allocation);
		}));
		_bufferPool.destroy(handle);
	}
	void CTX::destroy(InternalTextureHandle handle) {
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image) {
			return;
		}
		deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageView]() {
			vkDestroyImageView(device, imageView, nullptr);
		}));
		if (image->_vkImageViewStorage) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageViewStorage]() {
				vkDestroyImageView(device, imageView, nullptr);
			}));
		}
		// necessary for swapchain imges which swapchain is created from
		if (!image->_isOwning) {
			_texturePool.destroy(handle);
			_awaitingCreation = true;
			return;
		}
		if (image->_mappedPtr) {
			vmaUnmapMemory(_vmaAllocator, image->_vmaAllocation);
		}
		deferTask(std::packaged_task<void()>([vma = _vmaAllocator, image = image->_vkImage, allocation = image->_vmaAllocation]() {
			vmaDestroyImage(vma, image, allocation);
		}));
		_texturePool.destroy(handle);
		_awaitingCreation = true;
	}
	void CTX::destroy(InternalSamplerHandle handle) {
		AllocatedSampler* sampler = _samplerPool.get(handle);
		deferTask(std::packaged_task<void()>([device = _vkDevice, sampler = sampler->_vkSampler]() {
			vkDestroySampler(device, sampler, nullptr);
		}));
		_samplerPool.destroy(handle);
	}
	void CTX::destroy(InternalShaderHandle handle) {
		Shader* shader = _shaderPool.get(handle);
		deferTask(std::packaged_task<void()>([device = _vkDevice, module = shader->vkShaderModule]() {
			vkDestroyShaderModule(device, module, nullptr);
		}));
//		deferTask(std::packaged_task<void()>([device = _vkDevice, dss = shader->vkDSLs]() {
//			for (auto ds : dss) {
//				vkFreeDescriptorSets(device, ds, nullptr);
//			}
//		}));
		_shaderPool.destroy(handle);
	}
	void CTX::destroy(InternalPipelineHandle handle) {
		GraphicsPipeline* rps = _graphicsPipelinePool.get(handle);
		if (!rps) {
			return;
		}
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = rps->_vkPipeline]() {
			vkDestroyPipeline(device, pipeline, nullptr);
		}));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = rps->_vkPipelineLayout]() {
			vkDestroyPipelineLayout(device, layout, nullptr);
		}));
		_graphicsPipelinePool.destroy(handle);
	}

}