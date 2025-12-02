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

	void DescriptorSetWriter::updateBinding(mythril::InternalBufferHandle bufHandle, const char* name) {
		ASSERT_MSG(this->currentPipeline, "You must call updateBinding within opening and submitting a DescriptorSetWriter!");

		for (size_t i = 0; i < this->currentPipeline->signature.setSignatures.size(); i++) {
			const DescriptorSetSignature& set_signature = this->currentPipeline->signature.setSignatures[i];
			auto it = set_signature.nameToBinding.find(name);
			if (it != set_signature.nameToBinding.end()) {
				updateBinding(bufHandle, static_cast<int>(i), static_cast<int>(it->second));
				return;
			}
		}
		ASSERT_MSG(false, "Variable name '{}' could not be found in pipeline '{}'!", name, this->currentPipeline->_debugName);
	}

	void DescriptorSetWriter::updateBinding(InternalBufferHandle bufHandle, int set, int binding) {
		ASSERT_MSG(this->currentPipeline, "You must call updateBinding within opening and submitting a DescriptorSetWriter!");

		ASSERT_MSG(bufHandle.valid(), "Handle must be for a valid buffer object!");
		AllocatedBuffer* buf = _ctx->_bufferPool.get(bufHandle);
		ASSERT_MSG(buf->isUniformBuffer(), "Buffer passed to be written to descriptor binding must be uniform, aka uses 'BufferUsageBits_Uniform'!");
		ASSERT_MSG(buf->_bufferSize > 0, "Buffer size must be greater than 0!");

		const DescriptorSetSignature& set_signature = currentPipeline->signature.setSignatures[set];
		VkDescriptorSet vkset = currentPipeline->_managedDescriptorSets[set].vkDescriptorSet;
		ASSERT_MSG(vkset != VK_NULL_HANDLE, "VkDescriptorSet gathered is NULL!");
		this->writer.writeBuffer(vkset, binding, buf->_vkBuffer, buf->_bufferSize, 0, set_signature.bindings[binding].descriptorType);
	}

	void DWriter::writeBuffer(VkDescriptorSet set, unsigned int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type) {
		VkDescriptorBufferInfo& info = this->_bufferInfos.emplace_back(VkDescriptorBufferInfo{
				.buffer = buffer,
				.offset = offset,
				.range = size
		});

		VkWriteDescriptorSet write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext = nullptr,
				.dstSet = set,
				.dstBinding = static_cast<uint32_t>(binding),
				.descriptorCount = 1,
				.descriptorType = type,
				.pBufferInfo = &info
		};
		this->_writes.push_back(write);
	}
	void DWriter::updateSets(VkDevice device) {
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(_writes.size()), _writes.data(), 0, nullptr);
	}

	static VkMemoryPropertyFlags StorageTypeToVkMemoryPropertyFlags(StorageType storage) {
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

	static bool ValidateRange(const VkExtent3D& extent3D, uint32_t numLevels, const TexRange& range) {
		if (range.dimensions.width <= 0 ||
			range.dimensions.height <= 0 ||
			range.dimensions.depth <= 0 ||
			range.numLayers <= 0 ||
			range.numMipLevels <= 0) {
			LOG_SYSTEM(LogType::Error, "Values like: width, height, depth, numLayers and mipLevel must all be at least greater than 0.");
			return false;
		}
		if (range.mipLevel > numLevels) {
			LOG_SYSTEM(LogType::Error, "Requested mipLevels exceed texture's mipLevels!");
			return false;
		}
		const uint32_t texWidth = std::max(extent3D.width >> range.mipLevel, 1u);
		const uint32_t texHeight = std::max(extent3D.height >> range.mipLevel, 1u);
		const uint32_t texDepth = std::max(extent3D.depth >> range.mipLevel, 1u);

		if (range.dimensions.width > texWidth ||
			range.dimensions.height > texHeight ||
			range.dimensions.depth > texDepth) {
			LOG_SYSTEM(LogType::Error, "Range dimensions exceed texture dimensions!");
			return false;
		}
		if (range.offset.x > texWidth - range.dimensions.width ||
			range.offset.y > texHeight - range.dimensions.height ||
			range.offset.z > texDepth - range.dimensions.depth) {
			LOG_SYSTEM(LogType::Error, "Range dimensions exceed texture dimensions when accounting for offsets!");
			return false;
		}
		return true;
	}

	void CTX::construct() {
		ASSERT(this->_vkInstance != VK_NULL_HANDLE);
		ASSERT(this->_vkPhysicalDevice != VK_NULL_HANDLE);
		ASSERT(this->_vkDevice != VK_NULL_HANDLE);

		// ACTUAL INIT
		this->_imm = std::make_unique<ImmediateCommands>(this->_vkDevice, this->_graphicsQueueFamilyIndex);
		this->_staging = std::make_unique<StagingDevice>(*this);
		// DEFAULT VULKAN OBJECTS
		{
			// pattern xor
			const uint32_t texWidth = 256;
			const uint32_t texHeight = 256;
			std::vector<uint32_t> pixels(texWidth * texHeight);
			for (uint32_t y = 0; y != texHeight; y++) {
				for (uint32_t x = 0; x != texWidth; x++) {
					pixels[y * texWidth + x] =
							0xFF000000 + ((x ^ y) << 16) + ((x ^ y) << 8) + (x ^ y);
				}
			}
			// now take the pattern and create the first texture, the default and fallback texture for missing textures
			this->_dummyTextureHandle = this->createTexture({
				.dimension = {texWidth, texHeight},
				.usage = TextureUsageBits::TextureUsageBits_Sampled | TextureUsageBits::TextureUsageBits_Storage,
				.format = VK_FORMAT_R8G8B8A8_UNORM,
				.initialData = pixels.data(),
				.debugName = "Dummy Texture"
			});
			this->_dummyLinearSamplerHandle = this->createSampler({
				.magFilter = SamplerFilter::Linear,
				.minFilter = SamplerFilter::Linear,
				.wrapU = SamplerWrap::Clamp,
				.wrapV = SamplerWrap::Clamp,
				.wrapW = SamplerWrap::Clamp,
				.mipMap = SamplerMip::Disabled,
				.debugName = "Linear Sampler"
			});

			// swapchain must be built after default texture has been made
			// or else the fallback texture is the swapchain's texture
			VkExtent2D framebufferSize = this->getWindow().getFramebufferSize();
			this->_swapchain = std::make_unique<Swapchain>(*this, framebufferSize.width, framebufferSize.height);
			// timeline semaphore is closely kept to vulkan swapchain
			this->_timelineSemaphore = vkutil::CreateTimelineSemaphore(this->_vkDevice,
																	   this->_swapchain->getNumOfSwapchainImages() - 1);
			this->growBindlessDescriptorPoolImpl(this->_currentMaxTextureCount, this->_currentMaxSamplerCount);

			// https://vkguide.dev/docs/new_chapter_4/descriptor_abstractions/#:~:text=the%20end%20of-,init_descriptors,-()
			std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes = {
					{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
					{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
			};
			this->_descriptorAllocator.initialize(this->_vkDevice, 1000, frame_sizes);
		}
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
		destroy(_dummyLinearSamplerHandle);


		// TODO::fixing the allocation not being proerly freed for VMA
		if (_shaderPool.numObjects()) {
			LOG_SYSTEM(LogType::Info, "Cleaned up {} shader modules", _shaderPool.numObjects());
			for (int i = 0; i < _shaderPool._objects.size(); i++) {
				destroy(_shaderPool.getHandle(_shaderPool.findObject(&_shaderPool._objects[i]._obj).index()));
			}
		}
		if (_graphicsPipelinePool.numObjects()) {
			LOG_SYSTEM(LogType::Info, "Cleaned up {} render pipelines", _graphicsPipelinePool.numObjects());
			for (int i = 0; i < _graphicsPipelinePool._objects.size(); i++) {
				destroy(_graphicsPipelinePool.getHandle(_graphicsPipelinePool.findObject(&_graphicsPipelinePool._objects[i]._obj).index()));
			}
		}
		if (_samplerPool.numObjects() > 1) {
			// the dummy value is owned by the context
			LOG_SYSTEM(LogType::Info, "Cleaned up {} samplers", _samplerPool.numObjects() - 1);
			for (int i = 0; i < _samplerPool._objects.size(); i++) {
				destroy(_samplerPool.getHandle(_samplerPool.findObject(&_samplerPool._objects[i]._obj).index()));
			}
		}
		if (_texturePool.numObjects()) {
			LOG_SYSTEM(LogType::Info, "Cleaned up {} textures", _texturePool.numObjects());
			for (int i = 0; i < _texturePool._objects.size(); i++) {
				destroy(_texturePool.getHandle(_texturePool.findObject(&_texturePool._objects[i]._obj).index()));
			}
		}
		if (_bufferPool.numObjects()) {
			LOG_SYSTEM(LogType::Info, "Cleaned up {} buffers", _bufferPool.numObjects());
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
		_slangCompiler.destroy();

		_descriptorAllocator.destroyPools(_vkDevice);

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
			LOG_DEBUG("New Swapchain Extent: {} x {}", res.width, res.height);

			VK_CHECK(vkDeviceWaitIdle(_vkDevice));
			_swapchain.reset(nullptr);
			vkDestroySemaphore(_vkDevice, _timelineSemaphore, nullptr);
			_swapchain = std::make_unique<Swapchain>(*this, res.width, res.height);
			_timelineSemaphore = vkutil::CreateTimelineSemaphore(_vkDevice, _swapchain->getNumOfSwapchainImages()-1);
		} else {
			LOG_SYSTEM(LogType::Warning, "Cleaning (resizing) of Swapchain called when Swapchain is not dirty, ignoring.");
		}
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

	void CTX::checkAndUpdateBindlessDescriptorSetImpl() {
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
			growBindlessDescriptorPoolImpl(newMaxSamplers, newMaxTextures);
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

	void CTX::growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount) {
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
	VkDeviceAddress CTX::gpuAddress(InternalBufferHandle handle, size_t offset) {
		const AllocatedBuffer* buf = _bufferPool.get(handle);
		ASSERT_MSG(buf && buf->_vkDeviceAddress, "Buffer doesnt have a valid device address!");
		return buf->_vkDeviceAddress + offset;
	}

	LayoutBuildResult CTX::buildDescriptorResultFromSignature(const PipelineLayoutSignature& pipelineSignature) {
		LayoutBuildResult result;
		unsigned int max_set_size = pipelineSignature.setSignatures.size();
		result.allLayouts.reserve(max_set_size);
		result.allocatableLayouts.reserve(max_set_size);
		for (const DescriptorSetSignature& setSignature : pipelineSignature.setSignatures) {
			if (setSignature.isBindless) {
				result.allLayouts.push_back(_vkBindlessDSL);
				continue;
			}
			const VkDescriptorSetLayoutCreateInfo dsl_ci = {
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
					.pNext = nullptr,
					.flags = 0,
					.bindingCount = static_cast<uint32_t>(setSignature.bindings.size()),
					.pBindings = setSignature.bindings.data(),
			};

			VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
			VK_CHECK(vkCreateDescriptorSetLayout(_vkDevice, &dsl_ci, nullptr, &descriptor_set_layout));
			ASSERT_MSG(descriptor_set_layout != VK_NULL_HANDLE, "Descriptor Set Layout failed to allocate!");
			result.allLayouts.push_back(descriptor_set_layout);
			result.allocatableLayouts.push_back(descriptor_set_layout);
		}
		return result;
	}

	std::vector<VkDescriptorSet> CTX::allocateDescriptorSets(const std::vector<VkDescriptorSetLayout>& layouts) {
		std::vector<VkDescriptorSet> descriptor_sets;
		descriptor_sets.reserve(layouts.size());
		for (VkDescriptorSetLayout layout : layouts) {
			descriptor_sets.push_back(this->_descriptorAllocator.allocateSet(_vkDevice, layout));
		}
		return descriptor_sets;
	}
	VkPipelineLayout CTX::buildPipelineLayout(const std::vector<VkDescriptorSetLayout>& layouts, const std::vector<VkPushConstantRange>& ranges) {
		const VkPipelineLayoutCreateInfo pipeline_layout_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = static_cast<uint32_t>(layouts.size()),
				.pSetLayouts = layouts.data(),
				.pushConstantRangeCount = static_cast<uint32_t>(ranges.size()),
				.pPushConstantRanges =  ranges.data(),
		};
		VkPipelineLayout plLayout = VK_NULL_HANDLE;
		VK_CHECK(vkCreatePipelineLayout(_vkDevice, &pipeline_layout_info, nullptr, &plLayout));
		return plLayout;
	}

	PipelineLayoutSignature MergeSignatures(const std::vector<PipelineLayoutSignature>& pipelineSignatures) {
		// the signature that will be returned
		PipelineLayoutSignature out;
		std::unordered_map<uint32_t, DescriptorSetSignature> setMap;

		for (const PipelineLayoutSignature& sig : pipelineSignatures) {
			for (const DescriptorSetSignature& src : sig.setSignatures) {
				auto& dst = setMap[src.setIndex];

				// initialize once
				if (dst.bindings.empty() && !dst.isBindless) {
					dst.setIndex = src.setIndex;
					dst.isBindless = src.isBindless;
					dst.nameToBinding = src.nameToBinding;
				} else {
					for (const auto& [name, binding] : src.nameToBinding) {
						if (dst.nameToBinding.find(name) == dst.nameToBinding.end()) {
							dst.nameToBinding[name] = binding;
						}
					}
				}
				// if any other set is marked bindless
				dst.isBindless = dst.isBindless || src.isBindless;

				// merge bindings
				std::unordered_map<uint32_t, size_t> bindingIndexMap;
				bindingIndexMap.reserve(dst.bindings.size());
				for (size_t i = 0; i < dst.bindings.size(); ++i) {
					bindingIndexMap[dst.bindings[i].binding] = i;
				}
				for (const VkDescriptorSetLayoutBinding& bnd : src.bindings) {
					auto it = bindingIndexMap.find(bnd.binding);
					if (it != bindingIndexMap.end()) {
						auto& existing = dst.bindings[it->second];
						existing.stageFlags |= bnd.stageFlags;
						ASSERT_MSG(existing.descriptorType == bnd.descriptorType,
								   "Descriptor type mismatch for set={}, binding={}",
								   src.setIndex, bnd.binding);
						ASSERT_MSG(existing.descriptorCount == bnd.descriptorCount,
								   "Descriptor count mismatch for set={}, binding={}",
								   src.setIndex, bnd.binding);
					} else {
						dst.bindings.push_back(bnd);
					}
				}
			}
		}
		// sort merged sets by index
		out.setSignatures.reserve(setMap.size());
		for (auto& [index, sig] : setMap) {
			// sort bindings by binding index
			std::sort(sig.bindings.begin(), sig.bindings.end(),
					  [](const auto& x, const auto& y) { return x.binding < y.binding; });
			out.setSignatures.push_back(std::move(sig));
		}

		std::sort(out.setSignatures.begin(), out.setSignatures.end(),
				  [](const auto& a, const auto& b) { return a.setIndex < b.setIndex; });

		// 2. merge push constant func
		// apply merge func to every pushes by reference
		for (const PipelineLayoutSignature& sig : pipelineSignatures) {
			for (const VkPushConstantRange& src : sig.pushes) {
				bool found = false;

				for (auto& dst : out.pushes) {
					// check if the block we are comparing is actually the same where it matters (size & offset)
					if (dst.offset == src.offset && dst.size == src.size) {
						dst.stageFlags |= src.stageFlags;
						found = true;
						break;
					}
				}

				if (!found) {
					// check for overlaps between multiple push constants, vulkan spec disallows this
					for (const auto& dst : out.pushes) {
						bool overlap = !(src.offset + src.size <= dst.offset ||
										 dst.offset + dst.size <= src.offset);
						ASSERT_MSG(!overlap, "Push constant ranges overlap but differ in size or offset.");
					}
					out.pushes.push_back(src);
				}
			}
		}
		return out;
	}

	GraphicsPipeline* CTX::resolveRenderPipeline(InternalGraphicsPipelineHandle handle) {
		GraphicsPipeline* graphics_pipeline = _graphicsPipelinePool.get(handle);
		if (!graphics_pipeline) {
			LOG_SYSTEM(LogType::Warning, "Graphics pipeline does not exist, use a valid handle!");
			return VK_NULL_HANDLE;
		}

		checkAndUpdateBindlessDescriptorSetImpl();

		// updating descriptor layout //
//		if (graphics_pipeline->_vkLastDescriptorSetLayout != _vkBindlessDSL) {
//			deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = graphics_pipeline->_vkPipeline]() {
//				vkDestroyPipeline(device, pipeline, nullptr);
//			}));
//			deferTask(std::packaged_task<void()>([device = _vkDevice, layout = graphics_pipeline->_vkPipelineLayout]() {
//				vkDestroyPipelineLayout(device, layout, nullptr);
//			}));
//			graphics_pipeline->_vkPipeline = VK_NULL_HANDLE;
//			graphics_pipeline->_vkLastDescriptorSetLayout = _vkBindlessDSL;
//		}

		// RETURN EXISTING PIPELINE //
		if (graphics_pipeline->_vkPipeline != VK_NULL_HANDLE) {
			return graphics_pipeline;
		}
		// or, CREATE NEW PIPELINE //

		GraphicsPipelineSpec& spec = graphics_pipeline->_spec;

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
		for (const auto& colorAttachment : current_pass_info.colorAttachments) {
			colorFormats.push_back(colorAttachment.imageFormat);
		}
		builder.set_color_formats(colorFormats);
		if (current_pass_info.depthAttachment.has_value()) {
			builder.set_depth_format(current_pass_info.depthAttachment->imageFormat);
		}

		// shader setup & signature collection
		std::vector<PipelineLayoutSignature> pipeline_layout_signatures = {};
		ShaderStage& vertStage = spec.vertexShader;
		if (vertStage.valid()) {
			if (!vertStage.entryPoint) {
				vertStage.entryPoint = "vs_main";
			}
			Shader* shader = _shaderPool.get(vertStage.handle);
			builder.add_shader_module(shader->vkShaderModule, VK_SHADER_STAGE_VERTEX_BIT, vertStage.entryPoint);
			pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		}
		ShaderStage& fragStage = spec.fragmentShader;
		if (fragStage.valid()) {
			if (!fragStage.entryPoint) {
				fragStage.entryPoint = "fs_main";
			}
			Shader* shader = _shaderPool.get(fragStage.handle);
			builder.add_shader_module(shader->vkShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT, fragStage.entryPoint);
			pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		}
		ShaderStage& geometryStage = spec.geometryShader;
		if (geometryStage.valid()) {
			if (!geometryStage.entryPoint) {
				geometryStage.entryPoint = "gs_main";
			}
			Shader* shader = _shaderPool.get(geometryStage.handle);
			builder.add_shader_module(shader->vkShaderModule, VK_SHADER_STAGE_GEOMETRY_BIT, geometryStage.entryPoint);
			pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		}

		const PipelineLayoutSignature merged_pl_signature = MergeSignatures(pipeline_layout_signatures);

		const LayoutBuildResult result = buildDescriptorResultFromSignature(merged_pl_signature);
		VkPipelineLayout vk_pipeline_layout = buildPipelineLayout(result.allLayouts, merged_pl_signature.pushes);
		const std::vector<VkDescriptorSet> vk_descriptor_sets = allocateDescriptorSets(result.allocatableLayouts);

		// convert data into the managed stuct
		unsigned int managed_sets_size = result.allocatableLayouts.size();
		graphics_pipeline->_managedDescriptorSets.reserve(managed_sets_size);
		for (int i = 0; i < managed_sets_size; i++) {
			graphics_pipeline->_managedDescriptorSets.push_back({ vk_descriptor_sets[i], result.allocatableLayouts[i] });
		}
		// constructs another vector of descriptor sets but with the special bindless set in the correct index
		// this vector should ONLY be used by cmdBindPipeline which also calls a vkCmdBindDescriptorSets
		unsigned int total_sets_size = result.allLayouts.size();
		graphics_pipeline->_vkBindableDescriptorSets.reserve(total_sets_size);
		for (int i = 0; i < total_sets_size; i++) {
			if (merged_pl_signature.setSignatures[i].isBindless) {
				graphics_pipeline->_vkBindableDescriptorSets.push_back(this->_vkBindlessDSet);
				continue;
			}
			graphics_pipeline->_vkBindableDescriptorSets.push_back(vk_descriptor_sets[i]);
		}

		graphics_pipeline->signature = merged_pl_signature;
		graphics_pipeline->_vkPipeline = builder.build(_vkDevice, vk_pipeline_layout);
		graphics_pipeline->_vkPipelineLayout = vk_pipeline_layout;

		return graphics_pipeline;
	}

	VkDescriptorPool DescriptorAllocatorGrowable::createPoolImpl(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios) {
		std::vector<VkDescriptorPoolSize> pool_sizes;
		pool_sizes.reserve(this->_ratios.size());
		for (const PoolSizeRatio& ratio : poolRatios) {
			pool_sizes.push_back(VkDescriptorPoolSize{
					.type = ratio.type,
					.descriptorCount = static_cast<uint32_t>(ratio.ratio * (float)setCount)
			});
		}
		VkDescriptorPoolCreateInfo dsp_ci = {};
		dsp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dsp_ci.flags = 0;
		dsp_ci.maxSets = setCount;
		dsp_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
		dsp_ci.pPoolSizes = pool_sizes.data();

		VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
		vkCreateDescriptorPool(device, &dsp_ci, nullptr, &descriptor_pool);
		return descriptor_pool;
	}

	VkDescriptorPool DescriptorAllocatorGrowable::getPoolImpl(VkDevice device) {
		VkDescriptorPool new_pool = VK_NULL_HANDLE;
		if (!this->_readyPools.empty()) {
			new_pool = this->_readyPools.back();
			_readyPools.pop_back();
		} else {
			new_pool = this->createPoolImpl(device, this->_setsPerPool, this->_ratios);
			this->_setsPerPool *= 2;

			if (this->_setsPerPool > 4092) {
				this->_setsPerPool = 4092;
			}
		}
		ASSERT_MSG(new_pool != VK_NULL_HANDLE, "Cant get a null pool!");
		return new_pool;
	}
	void DescriptorAllocatorGrowable::initialize(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios) {
		this->_ratios.clear();
		for (auto& ratio : poolRatios) {
			this->_ratios.push_back(ratio);
		}
		VkDescriptorPool new_pool = this->createPoolImpl(device, initialSets, poolRatios);
		this->_setsPerPool = initialSets * 2;
		this->_readyPools.push_back(new_pool);
	}
	void DescriptorAllocatorGrowable::clearPools(VkDevice device) {
		for (auto ready_pool : this->_readyPools) {
			vkResetDescriptorPool(device, ready_pool, 0);
		}
		for (auto full_pool : this->_fullPools) {
			vkResetDescriptorPool(device, full_pool, 0);
			_readyPools.push_back(full_pool);
		}
		this->_fullPools.clear();
	}
	void DescriptorAllocatorGrowable::destroyPools(VkDevice device) {
		for (auto ready_pool : this->_readyPools) {
			vkDestroyDescriptorPool(device, ready_pool, nullptr);
		}
		this->_readyPools.clear();
		for (auto full_pool : this->_fullPools) {
			vkDestroyDescriptorPool(device, full_pool, nullptr);
		}
		this->_fullPools.clear();
	}
	VkDescriptorSet DescriptorAllocatorGrowable::allocateSet(VkDevice device, VkDescriptorSetLayout layout, void* pNext) {
		VkDescriptorPool pool_to_use = this->getPoolImpl(device);

		VkDescriptorSetAllocateInfo ds_ai = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.pNext = pNext,
				.descriptorPool = pool_to_use,
				.descriptorSetCount = 1,
				.pSetLayouts = &layout
		};
		VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
		VkResult result = vkAllocateDescriptorSets(device, &ds_ai, &descriptor_set);
		if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
			_fullPools.push_back(pool_to_use);
			pool_to_use = this->getPoolImpl(device);
			ds_ai.descriptorPool = pool_to_use;
			VK_CHECK(vkAllocateDescriptorSets(device, &ds_ai, &descriptor_set));
		}
		this->_readyPools.push_back(pool_to_use);
		return descriptor_set;
	}


	void CTX::generateMipmaps(InternalTextureHandle handle) {
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Generate mipmap request with empty handle!");
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
		if (spec.initialData) {
			upload(handle, spec.initialData, spec.size);
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
			LOG_SYSTEM(LogType::Warning, "The number of mip levels specified must be greater than 0!");
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
		if (spec.initialData) {
			ASSERT(spec.dataNumMipLevels <= spec.numMipLevels);
			ASSERT(spec.type == TextureType::Type_2D || spec.type == TextureType::Type_Cube);
			TexRange range = {
					.dimensions = extent3D,
					.numLayers = static_cast<uint32_t>((spec.type == TextureType::Type_Cube) ? 6 : 1),
					.numMipLevels = spec.dataNumMipLevels
			};
			this->upload(handle, spec.initialData, range);
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


	InternalGraphicsPipelineHandle CTX::createGraphicsPipeline(GraphicsPipelineSpec spec) {
		// all creating a pipeline does is assign the spec to it
		// actual construction is done upon first use (lazily)
		GraphicsPipeline obj = {};
		obj._spec = std::move(spec);
		// _debugName
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);
		InternalGraphicsPipelineHandle handle = _graphicsPipelinePool.create(std::move(obj));
		return handle;
	}

	InternalShaderHandle CTX::createShader(ShaderSpec spec) {
		// lazily create slang session & slang global session
		if (!_slangCompiler.sessionExists()) {
			_slangCompiler.create();
		}
		CompileResult compile_result = _slangCompiler.compileFile(spec.filePath);
		// TODO: this is some of the worst code i have ever written in my life, i am so sorry future me who will come back here and have to clean it
		Shader obj;
		// _debugName
		snprintf(obj._debugName, sizeof(obj._debugName) - 1, "%s", spec.debugName);

		ReflectionResult reflection_result = ReflectSPIRV(compile_result.getSpirvCode(), compile_result.getSpirvSize());
		obj._pipelineSignature = reflection_result.pipelineLayoutSignature;
		obj._descriptorSets = std::move(reflection_result.retrievedDescriptorSets);
		obj._pushConstants = std::move(reflection_result.retrivedPushConstants);

		// _vkShaderModule
		VkShaderModuleCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pNext = nullptr };
		create_info.flags = 0;
		create_info.pCode = compile_result.getSpirvCode();
		create_info.codeSize = compile_result.getSpirvSize();
		VkShaderModule shaderModule = VK_NULL_HANDLE;
		// after vkCreateShaderModule we no longer need CompileResult btw
		VK_CHECK(vkCreateShaderModule(_vkDevice, &create_info, nullptr, &shaderModule));
		obj.vkShaderModule = shaderModule;

		return _shaderPool.create(std::move(obj));
	}

	void CTX::upload(InternalBufferHandle handle, const void* data, size_t size, size_t offset) {
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		ASSERT_MSG(size > 0, "Size must be greater than 0!");

		AllocatedBuffer* buffer = _bufferPool.get(handle);

		if (offset + size > buffer->_bufferSize) {
			LOG_SYSTEM(LogType::Error, "Buffer request to upload is out of range! (Either the uploaded data size exceeds the size of the actual buffer or its offset is exceeding the total range)");
			return;
		}
		_staging->bufferSubData(*buffer, offset, size, data);
	}
	void CTX::download(InternalBufferHandle handle, void* data, size_t size, size_t offset) {
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Data is null");
			return;
		}
		AllocatedBuffer* buffer = _bufferPool.get(handle);

		if (!buffer) {
			LOG_SYSTEM(LogType::Error, "Retrieved buffer is null, handle must have been invalid!");
			return;
		}
		if (offset + size <= buffer->_bufferSize) {
			LOG_SYSTEM(LogType::Error, "Buffer request to download is out of range!");
			return;
		}
		buffer->getBufferSubData(*this, offset, size, data);
	}
	void CTX::upload(InternalTextureHandle handle, const void* data, const TexRange& range) {
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		ASSERT_MSG(image, "Attempting to use texture via invalid handle!");
		if (!ValidateRange(image->_vkExtent, image->_numLevels, range)) {
			LOG_SYSTEM(LogType::Warning, "Image failed validation check!");
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
			LOG_SYSTEM(LogType::Warning, "Data is null.");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image) {
			LOG_SYSTEM(LogType::Error, "Retrieved image is null, handle must have been invalid!");
			return;
		}
		if (!ValidateRange(image->_vkExtent, image->_numLevels, range)) {
			LOG_SYSTEM(LogType::Warning, "Image validation failed!");
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
		_shaderPool.destroy(handle);
	}
	void CTX::destroy(InternalGraphicsPipelineHandle handle) {
		GraphicsPipeline* graphics_pipeline = _graphicsPipelinePool.get(handle);
		if (!graphics_pipeline) {
			return;
		}
		deferTask(std::packaged_task<void()>([device = _vkDevice, managedlayouts = graphics_pipeline->_managedDescriptorSets]() {
			for (const auto& managedlayout : managedlayouts) {
				vkDestroyDescriptorSetLayout(device, managedlayout.vkDescriptorSetLayout, nullptr);
			}
		}));
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = graphics_pipeline->_vkPipeline]() {
			vkDestroyPipeline(device, pipeline, nullptr);
		}));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = graphics_pipeline->_vkPipelineLayout]() {
			vkDestroyPipelineLayout(device, layout, nullptr);
		}));
		_graphicsPipelinePool.destroy(handle);
	}

}