//
// Created by Hayden Rivas on 10/6/25.
//

#include "mythril/CTX.h"
#include "RenderGraphInternal.h"

#include <iostream>
#include <ranges>

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "Pipelines.h"
#include "Plugins.h"
#include "mythril/CTXBuilder.h"
#include "mythril/Objects.h"
#include "vkstring.h"
#include "vkutil.h"

#include <slang/slang-com-ptr.h>
#include <slang/slang-cpp-types-core.h>
#include <slang/slang-cpp-types.h>
#include <slang/slang.h>

#include "AccelerationStructure.h"
#include "AccelerationStructure.h"
#include "CommandBuffer.h"

namespace mythril {
	using namespace detail;
	// we arent using VkMutable
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
			// we use 9 because 0-8 are assigned already by slang
			kAccelerationStructure = 9,

			// this MUST be manually updated
			kNumOfBinds = 4
		};
	} // namespace BindlessSpaceIndex

	static uint64_t getAlignedAddress(uint64_t addr, uint64_t align) {
		const uint64_t offs = addr % align;
		return offs ? addr + (align - offs) : addr;
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
		if (range.dimensions.width <= 0 || range.dimensions.height <= 0 || range.dimensions.depth <= 0 || range.numLayers <= 0 || range.numMipLevels <= 0) {
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

		if (range.dimensions.width > texWidth || range.dimensions.height > texHeight || range.dimensions.depth > texDepth) {
			LOG_SYSTEM(LogType::Error, "Range dimensions exceed texture dimensions!");
			return false;
		}
		if (range.offset.x > texWidth - range.dimensions.width || range.offset.y > texHeight - range.dimensions.height || range.offset.z > texDepth - range.dimensions.depth) {
			LOG_SYSTEM(LogType::Error, "Range dimensions exceed texture dimensions when accounting for offsets!");
			return false;
		}
		return true;
	}

	bool CTX::isExtensionEnabled(const std::string_view extensionName) const { return _enabledExtensionNames.contains(extensionName); }
	void CTX::construct() {
		ASSERT(this->_vkInstance != VK_NULL_HANDLE);
		ASSERT(this->_vkPhysicalDevice != VK_NULL_HANDLE);
		ASSERT(this->_vkDevice != VK_NULL_HANDLE);

		// ACTUAL INIT
		this->_immGraphics = std::make_unique<ImmediateCommands>(this->_vkDevice, this->_graphicsQueueFamilyIndex, this->_vkGraphicsQueue);
		if (_computeQueueFamilyIndex != _graphicsQueueFamilyIndex && _vkComputeQueue)
			this->_immAsyncCompute = std::make_unique<ImmediateCommands>(this->_vkDevice, this->_computeQueueFamilyIndex, this->_vkComputeQueue);
		if (_transferQueueFamilyIndex != _graphicsQueueFamilyIndex &&
			_transferQueueFamilyIndex != _computeQueueFamilyIndex && _vkTransferQueue)
			this->_immAsyncTransfer = std::make_unique<ImmediateCommands>(this->_vkDevice, this->_transferQueueFamilyIndex, this->_vkTransferQueue);

		this->_staging = std::make_unique<StagingDevice>(*this);
		// DEFAULT VULKAN OBJECTS
		{
			// pattern xor
			constexpr uint32_t texWidth = 256;
			constexpr uint32_t texHeight = 256;
			std::vector<uint32_t> pixels(texWidth * texHeight);
			for (uint32_t y = 0; y != texHeight; y++) {
				for (uint32_t x = 0; x != texWidth; x++) {
					pixels[y * texWidth + x] = 0xFF000000 + ((x ^ y) << 16) + ((x ^ y) << 8) + (x ^ y);
				}
			}
			// now take the pattern and create the first texture
			// fallback texture, index 0
			this->_dummyTexture = this->createTexture(
			        {.dimension = {texWidth, texHeight},
			         .format = VK_FORMAT_R8G8B8A8_UNORM,
			         .usage = TextureUsageBits::TextureUsageBits_Sampled | TextureUsageBits::TextureUsageBits_Storage,
			         .initialData = pixels.data(),
			         .debugName = "Internal_Dummy_Texture"}
			);
			// fallback sampler, index 0
			this->_dummyLinearSampler = this->createSampler(
			        {.magFilter = SamplerFilter::Linear,
			         .minFilter = SamplerFilter::Linear,
			         .wrapU = SamplerWrap::ClampEdge,
			         .wrapV = SamplerWrap::ClampEdge,
			         .wrapW = SamplerWrap::ClampEdge,
			         .debugName = "Internal_Linear_Sampler"}
			);
			// dummy tlas for bindless validation warnings
			if (isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
				constexpr VkDeviceSize kDummyTLASSize = 256;
				this->_dummyTLASBuffer = this->createBuffer({
					.size = kDummyTLASSize,
					.usage = BufferUsageBits_AccelerationStructureStorage,
					.storage = StorageType::Device,
					.debugName = "Internal_Dummy_TLAS_Buffer"
				});
				const VkAccelerationStructureCreateInfoKHR ci = {
					.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
					.buffer = _bufferPool.get(_dummyTLASBuffer.handle())->_vkBuffer,
					.size = kDummyTLASSize,
					.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
				};
				VK_CHECK(vkCreateAccelerationStructureKHR(_vkDevice, &ci, nullptr, &_dummyTLAS));
				vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(_dummyTLAS), "Internal_Dummy_TLAS");
			}
			// initial descriptor pool creation
			this->growBindlessDescriptorPoolImpl(this->_currentMaxSamplerCount, this->_currentMaxTextureCount, this->_currentMaxAccelerationCount);
		}
	}

	CTX::~CTX() {
		VK_CHECK(vkDeviceWaitIdle(this->_vkDevice));
		// more like awaitingDestruction :0
		this->_awaitingCreation = true;

#ifdef MYTH_ENABLED_IMGUI
		if (this->_imguiPlugin.isEnabled()) {
			this->_imguiPlugin.onDestroy();
		}
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		this->_tracyPlugin.onDestroy();
#endif

		destroy(this->_staging->_stagingBuffer);
		this->_staging.reset(nullptr);
		this->destroySwapchain();
		this->_dummyTexture.release();
		this->_dummyLinearSampler.release();
		if (_dummyTLAS != VK_NULL_HANDLE) {
			vkDestroyAccelerationStructureKHR(_vkDevice, _dummyTLAS, nullptr);
			_dummyTLAS = VK_NULL_HANDLE;
		}
		this->_dummyTLASBuffer.release();

		// TODO::fixing the allocation not being proerly freed for buffers VMA
		auto destroyAll = [this]<typename H, typename O>(HandlePool<H, O>& pool, const char* name) {
			if (!pool.numObjects())
				return;
			LOG_SYSTEM(LogType::Info, "Cleaned up {} {}", pool.numObjects(), name);
			for (uint32_t i = 0; i < pool._objects.size(); i++)
				destroy(pool.getHandle(i));
		};
		destroyAll(_shaderPool, "shader modules");
		destroyAll(_graphicsPipelinePool, "graphics pipelines");
		destroyAll(_computePipelinePool, "compute pipelines");
		destroyAll(_rayTracingPipelinePool, "raytracing pipelines");
		destroyAll(_samplerPool, "samplers");
		destroyAll(_texturePool, "textures");
		destroyAll(_bufferPool, "buffers");
		destroyAll(_accelerationStructurePool, "acceleration structures");


		// the order of operations is very specific so that we dont trigger any validation warnings

		// wipe buffers
		_bufferPool.clear();
		_texturePool.clear();
		_samplerPool.clear();
		_shaderPool.clear();
		_graphicsPipelinePool.clear();
		_computePipelinePool.clear();
		_rayTracingPipelinePool.clear();
		_accelerationStructurePool.clear();
		// make sure imm tasks are complete
		waitDeferredTasks();
		_immAsyncTransfer.reset(nullptr);
		_immAsyncCompute.reset(nullptr);
		_immGraphics.reset(nullptr);

		// destroy slang compiler
		_slangCompiler.destroy();

		// destroy our bindless descriptor stuff
		vkDestroyDescriptorSetLayout(_vkDevice, _vkBindlessDSL, nullptr);
		vkDestroyDescriptorPool(_vkDevice, _vkBindlessDPool, nullptr);

		vmaDestroyAllocator(_vmaAllocator);
		// destroy pure vk objects
		vkDestroyDevice(_vkDevice, nullptr);
#ifdef DEBUG
		vkDestroyDebugUtilsMessengerEXT(_vkInstance, _vkDebugMessenger, nullptr);
#endif
		if (this->_surfaceDestroyFunc) {
			this->_surfaceDestroyFunc(_vkInstance, _vkSurfaceKHR);
		}
		vkDestroyInstance(_vkInstance, nullptr);
	}

	void CTX::recreateSwapchain(const SwapchainSpec& spec) {
		VK_CHECK(vkDeviceWaitIdle(this->_vkDevice));
		destroySwapchain();
		createSwapchain(spec);
		// destroy+create already each bump _resourceEpoch; no additional bump needed here.
	}

	void CTX::destroySwapchain() {
		if (!_swapchain) {
			LOG_SYSTEM(LogType::Warning, "CTX::destroySwapchain called when no existing swapchain exists!");
			return;
		}
		this->_swapchain.reset(nullptr);
		vkDestroySemaphore(_vkDevice, this->_timelineSemaphore, nullptr);
		++_resourceEpoch;
	}

	template<typename T>
	static void AssignIfValid(T& dst, const T& src, T invalid) {
		if (src != invalid && dst != src)
			dst = src;
	}
	void CTX::createSwapchain(const SwapchainSpec& spec) {
		// does some checks for the user that the default constructor doesnt do
		if (_swapchain) {
			LOG_SYSTEM(LogType::Warning, "CTX::createSwapchain called when an already existing swapchain exists!");
			return;
		}
		SwapchainSpec new_swapchain_spec{};
		new_swapchain_spec = {
		    .width = lastSwapchainSpec.width,
		    .height = lastSwapchainSpec.height,
		    .format = lastSwapchainSpec.format,
		    .colorSpace = lastSwapchainSpec.colorSpace,
		    .presentMode = lastSwapchainSpec.presentMode,
		};
		AssignIfValid(new_swapchain_spec.width, spec.width, static_cast<uint32_t>(0));
		AssignIfValid(new_swapchain_spec.height, spec.height, static_cast<uint32_t>(0));
		AssignIfValid(new_swapchain_spec.format, spec.format, VK_FORMAT_MAX_ENUM);
		AssignIfValid(new_swapchain_spec.colorSpace, spec.colorSpace, VK_COLOR_SPACE_MAX_ENUM_KHR);
		AssignIfValid(new_swapchain_spec.presentMode, spec.presentMode, VK_PRESENT_MODE_MAX_ENUM_KHR);

		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot create a swapchain while a CommandBuffer is open, deferred tasks are flushed here.");
		VK_CHECK(vkDeviceWaitIdle(this->_vkDevice));
		// trivial win which opens the ability for the swapchian to reuse existing indicies that are now available
		waitDeferredTasks();
		this->_swapchain = std::make_unique<Swapchain>(*this, new_swapchain_spec);
		this->_timelineSemaphore = vkutil::CreateTimelineSemaphore(_vkDevice, this->_swapchain->getNumOfSwapchainImages() - 1);
		// we need to store in the situation where the swapcahin is deleted and we want to use the last swapchains settings
		lastSwapchainSpec = new_swapchain_spec;
		wrappedBackBuffer.updateHandle(*this, _swapchain->getCurrentSwapchainTextureHandle());
		++_resourceEpoch;
	}

	void CTX::checkAndUpdateBindlessDescriptorSetImpl() {
		MYTH_PROFILER_FUNCTION();
		if (!_awaitingCreation) {
			return;
		}
		uint32_t newMaxSamplers = _currentMaxSamplerCount;
		uint32_t newMaxTextures = _currentMaxTextureCount;
		uint32_t newMaxAccelStructures = _currentMaxAccelerationCount;

		while (_samplerPool._objects.size() > newMaxSamplers) {
			newMaxSamplers *= 2;
		}
		while (_texturePool._objects.size() > newMaxTextures) {
			newMaxTextures *= 2;
		}
		while (_accelerationStructurePool._objects.size() > newMaxAccelStructures) {
			newMaxAccelStructures *= 2;
		}
		if (newMaxSamplers != _currentMaxSamplerCount || newMaxTextures != _currentMaxTextureCount || newMaxAccelStructures != _currentMaxAccelerationCount) {
			// actual call site for growing
			this->growBindlessDescriptorPoolImpl(newMaxSamplers, newMaxTextures, newMaxAccelStructures);
			// pipeline layouts include the old DSL's descriptor counts so they are incompatible with the new set and must be rebuilt
			for (auto& obj : _graphicsPipelinePool._objects)
				invalidateBindlessPipelineImpl(obj._obj._shared);
			for (auto& obj : _computePipelinePool._objects)
				invalidateBindlessPipelineImpl(obj._obj._shared);
			for (auto& obj : _rayTracingPipelinePool._objects)
				invalidateBindlessPipelineImpl(obj._obj._shared);
		}
		// only reached when a slot write failed, so the target is a fresh set after a grow or one that was never bound
		_awaitingCreation = false;
		writeSamplerDescriptorsImpl(0, static_cast<uint32_t>(_samplerPool._objects.size()));
		writeTextureDescriptorsImpl(0, static_cast<uint32_t>(_texturePool._objects.size()));
		writeAccelerationStructureDescriptorsImpl(0, static_cast<uint32_t>(_accelerationStructurePool._objects.size()));
	}

	void CTX::writeTextureDescriptorsImpl(uint32_t first, uint32_t count) {
		if (_awaitingCreation || _vkBindlessDSet == VK_NULL_HANDLE || first + count > _currentMaxTextureCount) {
			_awaitingCreation = true;
			return;
		}
		if (count == 0)
			return;
		// our dummyTexture, the cross pattern, is the backup/resolve
		VkImageView dummyImageView = _texturePool._objects[0]._obj._vkImageView;
		// sampled infos first, storage infos after, so each binding gets one contiguous run
		std::vector<VkDescriptorImageInfo> infos(2 * count);
		for (uint32_t k = 0; k < count; ++k) {
			const AllocatedTexture& img = _texturePool._objects[first + k]._obj;
			// swapchain images must not be in bindless, after present, the drawable is returned to
			// already presenting this drawable on moltenvk
			// multisampled images cannot be directly accessed from shaders
			const bool isTextureAvailable = !img.isSwapchainImage() && (img.getSampleCount() & VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_1_BIT;
			VkImageView storageView = img._vkImageViewStorage ? img._vkImageViewStorage : img._vkImageView;
			infos[k] = {
				.sampler = VK_NULL_HANDLE,
				.imageView = isTextureAvailable && img.isSampledImage() ? img._vkImageView : dummyImageView,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			ASSERT_MSG(infos[k].imageView != VK_NULL_HANDLE, "Sampled imageView is null!");
			// storage images could be used for anything, per in general layout
			infos[count + k] = {
				.sampler = VK_NULL_HANDLE,
				.imageView = isTextureAvailable && img.isStorageImage() ? storageView : dummyImageView,
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
		}
		const VkWriteDescriptorSet writes[] = {
			VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = _vkBindlessDSet,
				.dstBinding = BindlessSpaceIndex::kSampledImage,
				.dstArrayElement = first,
				.descriptorCount = count,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				.pImageInfo = infos.data(),
			},
			VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = _vkBindlessDSet,
				.dstBinding = BindlessSpaceIndex::kStorageImage,
				.dstArrayElement = first,
				.descriptorCount = count,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo = infos.data() + count,
			},
		};
		vkUpdateDescriptorSets(_vkDevice, 2, writes, 0, nullptr);
	}

	void CTX::writeSamplerDescriptorsImpl(uint32_t first, uint32_t count) {
		if (_awaitingCreation || _vkBindlessDSet == VK_NULL_HANDLE || first + count > _currentMaxSamplerCount) {
			_awaitingCreation = true;
			return;
		}
		if (count == 0)
			return;
		// our linear sampler is the backup/resolve
		const VkSampler linearSampler = _samplerPool._objects[0]._obj._vkSampler;
		std::vector<VkDescriptorImageInfo> infos;
		infos.reserve(count);
		for (uint32_t i = first; i < first + count; ++i) {
			const VkSampler sampler = _samplerPool._objects[i]._obj._vkSampler;
			infos.push_back({.sampler = sampler ? sampler : linearSampler, .imageView = VK_NULL_HANDLE, .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED});
		}
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _vkBindlessDSet,
			.dstBinding = BindlessSpaceIndex::kSampler,
			.dstArrayElement = first,
			.descriptorCount = count,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
			.pImageInfo = infos.data(),
		};
		vkUpdateDescriptorSets(_vkDevice, 1, &write, 0, nullptr);
	}

	void CTX::writeAccelerationStructureDescriptorsImpl(uint32_t first, uint32_t count) {
		if (_awaitingCreation || _vkBindlessDSet == VK_NULL_HANDLE || first + count > _currentMaxAccelerationCount) {
			_awaitingCreation = true;
			return;
		}
		if (count == 0 || _dummyTLAS == VK_NULL_HANDLE)
			return;
		std::vector<VkAccelerationStructureKHR> handles;
		handles.reserve(count);
		for (uint32_t i = first; i < first + count; ++i) {
			const AllocatedAccelerationStructure& as = _accelerationStructurePool._objects[i]._obj;
			handles.push_back(as._isTLAS && as._vkAccelerationStructure != VK_NULL_HANDLE ? as._vkAccelerationStructure : _dummyTLAS);
		}
		const VkWriteDescriptorSetAccelerationStructureKHR writeAccelStruct = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = count,
			.pAccelerationStructures = handles.data(),
		};
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = &writeAccelStruct,
			.dstSet = _vkBindlessDSet,
			.dstBinding = BindlessSpaceIndex::kAccelerationStructure,
			.dstArrayElement = first,
			.descriptorCount = count,
			.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		};
		vkUpdateDescriptorSets(_vkDevice, 1, &write, 0, nullptr);
	}

	void CTX::growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount, uint32_t newMaxAccelStructureCount) {
		MYTH_PROFILER_FUNCTION();
		_currentMaxSamplerCount = newMaxSamplerCount;
		_currentMaxTextureCount = newMaxTextureCount;
		_currentMaxAccelerationCount = newMaxAccelStructureCount;

		const uint32_t MAX_TEXTURE_LIMIT = getPhysicalDeviceProperties12().maxDescriptorSetUpdateAfterBindSampledImages;
		ASSERT_MSG(newMaxTextureCount <= MAX_TEXTURE_LIMIT, "Max sampled textures exceeded: {}, but maximum of {} is allowed!", newMaxTextureCount, MAX_TEXTURE_LIMIT);

		const uint32_t MAX_SAMPLER_LIMIT = getPhysicalDeviceProperties12().maxDescriptorSetUpdateAfterBindSamplers;
		ASSERT_MSG(newMaxSamplerCount <= MAX_SAMPLER_LIMIT, "Max samplers exceeded: {}, but maximum of {} is allowed!", newMaxSamplerCount, MAX_SAMPLER_LIMIT);

		if (_vkBindlessDSL != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dsl = _vkBindlessDSL]() { vkDestroyDescriptorSetLayout(device, dsl, nullptr); }));
		}
		if (_vkBindlessDPool != VK_NULL_HANDLE) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, dp = _vkBindlessDPool]() { vkDestroyDescriptorPool(device, dp, nullptr); }));
		}

		VkShaderStageFlags stage_flags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
		if (isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
			stage_flags |= (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
						   VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR);
		}
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
			VkDescriptorSetLayoutBinding(BindlessSpaceIndex::kAccelerationStructure, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, newMaxAccelStructureCount, stage_flags, nullptr)
		};
		constexpr uint32_t dsbinding_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		// assign each of our descriptors the same flags
		VkDescriptorBindingFlags bindingFlags[BindlessSpaceIndex::kNumOfBinds];
		for (VkDescriptorBindingFlags& bindingFlag : bindingFlags) {
			bindingFlag = dsbinding_flags;
		}
		// we only have one conditional set so account for it during runtime here
		const uint32_t numberofBindsNeeded = isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ? BindlessSpaceIndex::kNumOfBinds : BindlessSpaceIndex::kNumOfBinds - 1;
		// ReSharper disable once CppVariableCanBeMadeConstexpr
		const VkDescriptorSetLayoutBindingFlagsCreateInfo dsl_bf_ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount = numberofBindsNeeded,
		    .pBindingFlags = bindingFlags,
		};
		const VkDescriptorSetLayoutCreateInfo dsl_ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .pNext = &dsl_bf_ci,
		    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		    .bindingCount =numberofBindsNeeded,
		    .pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(_vkDevice, &dsl_ci, nullptr, &_vkBindlessDSL));
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(_vkBindlessDSL), "Mythril's Bindless Descriptor SetLayout");

		// always just have as max possible length
		VkDescriptorPoolSize poolSizes[BindlessSpaceIndex::kNumOfBinds]{
		    VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_SAMPLER, newMaxSamplerCount),
		    //			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, newMaxTextureCount),
		    VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, newMaxTextureCount),
		    VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, newMaxTextureCount),
		    //			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, newMaxTextureCount),
		    //			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, newMaxTextureCount),
		    //			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, newMaxTextureCount),
		    //			VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, newMaxTextureCount),
		};
		uint32_t numPoolSizes = 3;
		if (isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
			poolSizes[numPoolSizes++] = VkDescriptorPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, newMaxAccelStructureCount);
		}
		const VkDescriptorPoolCreateInfo dp_ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		    .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
		    .maxSets = 1,
		    .poolSizeCount = numPoolSizes,
		    .pPoolSizes = poolSizes,
		};
		VK_CHECK(vkCreateDescriptorPool(_vkDevice, &dp_ci, nullptr, &_vkBindlessDPool));
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<uint64_t>(_vkBindlessDPool), "Mythril's Bindless Descriptor Pool");
		const VkDescriptorSetAllocateInfo ds_ai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = _vkBindlessDPool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &_vkBindlessDSL,
		};
		VK_CHECK(vkAllocateDescriptorSets(_vkDevice, &ds_ai, &_vkBindlessDSet));
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(_vkBindlessDSet), "Mythril's Bindless Descriptor Set");
	}

	VkDeviceAddress CTX::gpuAddress(BufferHandle handle, size_t offset) const {
		const AllocatedBuffer* buf = _bufferPool.get(handle);
		ASSERT_MSG(buf, "Buffer handle (index {}, gen {}) is empty or stale!", handle.index(), handle.gen());
		ASSERT_MSG(buf->_vkDeviceAddress, "Buffer '{}' doesnt have a valid device address!", buf->getDebugName());
		return buf->_vkDeviceAddress + offset;
	}
	VkDeviceAddress CTX::gpuAddress(AccelerationStructureHandle handle) const {
		const AllocatedAccelerationStructure* structure = _accelerationStructurePool.get(handle);
		ASSERT_MSG(structure, "Acceleration Structure handle (index {}, gen {}) is empty or stale!", handle.index(), handle.gen());
		ASSERT_MSG(structure->_vkDeviceAddress, "Acceleration Structure '{}' doesnt have a valid device address!", structure->getDebugName());
		return structure->_vkDeviceAddress;
	}

	static VkPipelineLayout BuildPipelineLayout(VkDevice device, const std::vector<VkDescriptorSetLayout>& layouts, const std::vector<VkPushConstantRange>& ranges) {
		const VkPipelineLayoutCreateInfo pipeline_layout_info = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = static_cast<uint32_t>(layouts.size()),
		    .pSetLayouts = layouts.data(),
		    .pushConstantRangeCount = static_cast<uint32_t>(ranges.size()),
		    .pPushConstantRanges = ranges.data(),
		};
		VkPipelineLayout plLayout = VK_NULL_HANDLE;
		VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &plLayout));
		return plLayout;
	}

	static PipelineLayoutSignature MergeSignatures(const std::vector<PipelineLayoutSignature>& pipelineSignatures) {
		PipelineLayoutSignature out;

		// 1. unify the bindless set index across all stages
		for (const PipelineLayoutSignature& sig: pipelineSignatures) {
			if (!sig.bindlessSetIndex.has_value())
				continue;
			if (!out.bindlessSetIndex.has_value()) {
				out.bindlessSetIndex = sig.bindlessSetIndex;
			} else {
				ASSERT_MSG(*out.bindlessSetIndex == *sig.bindlessSetIndex, "Pipeline stages disagree on bindless set index ({} vs {}).", *out.bindlessSetIndex, *sig.bindlessSetIndex);
			}
		}

		// 2. merge push constant func
		// apply merge func to every pushes by reference
		for (const PipelineLayoutSignature& sig: pipelineSignatures) {
			for (const VkPushConstantRange& src: sig.pushes) {
				bool found = false;

				for (auto& dst: out.pushes) {
					// check if the block we are comparing is actually the same where it matters (size & offset)
					if (dst.offset == src.offset && dst.size == src.size) {
						dst.stageFlags |= src.stageFlags;
						found = true;
						break;
					}
				}

				if (!found) {
					// check for overlaps between multiple push constants, vulkan spec disallows this
					for (const auto& dst: out.pushes) {
						bool overlap = !(src.offset + src.size <= dst.offset || dst.offset + dst.size <= src.offset);
						ASSERT_MSG(!overlap, "Push constant ranges overlap but differ in size or offset.");
					}
					out.pushes.push_back(src);
				}
			}
		}
		return out;
	}

	static int ResolveSpecializationConstantID(const std::variant<std::string, int>& identifier, const std::unordered_map<std::string, int>& map) {
		bool isString = std::holds_alternative<std::string>(identifier);
		if (!isString)
			return std::get<int>(identifier);
		const auto& name = std::get<std::string>(identifier);
		auto it = map.find(name);
		if (it != map.end()) {
			return it->second;
		}
		ASSERT_MSG(false, "Your specialization constant entry '{}' could not be found", name);
	}

	struct SpecializationInfoBundle {
		VkSpecializationInfo vkInfo{};
		// purely for lifetime reasons
		std::vector<VkSpecializationMapEntry> mapEntriesData;
		std::vector<std::byte> packedData;
	};
	static std::unique_ptr<SpecializationInfoBundle> BuildSpecializationInfoBundle(SpecializationConstantEntry* pEntries, uint32_t entryCount, const std::unordered_map<std::string, int>& map) {
		// if entryCount is 0 just save ourselves time
		if (entryCount < 1)
			return nullptr;
		if (map.empty())
			return nullptr;

		auto bundle = std::make_unique<SpecializationInfoBundle>();
		bundle->mapEntriesData.reserve(entryCount);

		size_t offset = 0;
		for (uint32_t i_entry = 0; i_entry < entryCount; i_entry++) {
			const SpecializationConstantEntry& spec_entry = pEntries[i_entry];
			VkSpecializationMapEntry vk_entry = {};
			vk_entry.size = spec_entry.size;
			vk_entry.offset = offset;
			vk_entry.constantID = ResolveSpecializationConstantID(spec_entry.identifier, map);
			bundle->mapEntriesData.push_back(vk_entry);
			offset += spec_entry.size;
		}

		// make packed structure
		bundle->packedData.resize(offset);
		for (uint32_t i = 0; i < entryCount; i++) {
			const SpecializationConstantEntry& spec_entry = pEntries[i];
			const size_t dstOffset = bundle->mapEntriesData[i].offset;
			std::memcpy(bundle->packedData.data() + dstOffset, spec_entry.data, spec_entry.size);
		}

		bundle->vkInfo.mapEntryCount = bundle->mapEntriesData.size();
		bundle->vkInfo.pMapEntries = bundle->mapEntriesData.data();
		bundle->vkInfo.dataSize = bundle->packedData.size();
		bundle->vkInfo.pData = bundle->packedData.data();

		return bundle;
	}

	PipelineCoreData CTX::buildPipelineCommonDataExceptVkPipelineImpl(const PipelineLayoutSignature& signature) {
		// because we are fully bindless, we have at most one descriptor set (the bindless one) is ever bound
		// however i want to leave this legacy code in here :)
		std::vector<VkDescriptorSetLayout> layouts;
		if (signature.bindlessSetIndex.has_value()) {
			// The bindless set must live at set index 0 because Vulkan's set-layout array is
			// positional. Slang assigns it as the highest set index by default; if the user has
			// other (non-bindless) sets the reflection step will already have errored out.
			ASSERT_MSG(*signature.bindlessSetIndex == 0, "Bindless set must be declared at set index 0, got {}.", *signature.bindlessSetIndex);
			layouts.push_back(_vkBindlessDSL);
		}

		VkPipelineLayout vk_pipeline_layout = BuildPipelineLayout(this->_vkDevice, layouts, signature.pushes);

		PipelineCoreData common;
		common._vkPipelineLayout = vk_pipeline_layout;
		common.signature = signature;
		common._vkBindlessDescriptorSet = signature.bindlessSetIndex.has_value() ? this->_vkBindlessDSet : VK_NULL_HANDLE;
		return common;
	}

	// graphics pipeline doesnt have one of these because that is abstracted inside the GraphicsPipelineBuilder class
	VkPipeline CTX::buildComputePipelineImpl(VkPipelineLayout layout, ComputePipelineSpec spec) {
		uint32_t sc_count = 0;
		while (sc_count < 16 && spec.specConstants[sc_count].size) {
			sc_count++;
		}
		// a compute pipeline only has one shader anyways
		AllocatedShader* shader = _shaderPool.get(spec.shader);
		CheckSpecializationConstants(*shader, sc_count, "compute", spec.debugName, false);
		const std::unique_ptr<SpecializationInfoBundle> spec_constants_bundle = BuildSpecializationInfoBundle(spec.specConstants, sc_count, shader->_specializationInfo.nameToID);

		VkPipelineShaderStageCreateInfo shader_stage_ci = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		    .pNext = nullptr,
		    .flags = 0,
		    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		    .module = shader->_vkShaderModule,
		    .pName = "cs_main",
		    .pSpecializationInfo = &spec_constants_bundle->vkInfo,
		};
		VkComputePipelineCreateInfo pipeline_ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = shader_stage_ci, .layout = layout};

		VkPipeline vk_pipeline = VK_NULL_HANDLE;
		VK_CHECK(vkCreateComputePipelines(_vkDevice, nullptr, 1, &pipeline_ci, nullptr, &vk_pipeline));
		return vk_pipeline;
	}

	void CTX::CheckSpecializationConstants(const AllocatedShader& shader, unsigned int expectedSpecConstantscount, const char* shaderStageName, const char* pipelineDebugName, bool hasMultipleStages) {
		// calculated from reflection of shader
		const unsigned int calculatedcount = shader._specializationInfo.specializationConstants.size();

		if (calculatedcount > expectedSpecConstantscount)
			LOG_SYSTEM(
					LogType::Warning, "You have specialization constants used in the {} shader '{}' that are not defined in the pipeline creation for '{}'!", shaderStageName, shader._debugName, pipelineDebugName
			);
		if (hasMultipleStages) return;
		// basically the opposite, and less common since the user instinctively wouldnt be including specialization constants in the construction of their pipline without planning how it would look in the shader
		if (calculatedcount < expectedSpecConstantscount)
			LOG_SYSTEM(
					LogType::Warning, "You have specialization constants defined in the pipeline creation for '{}' but do not exist in the {} shader '{}'!", pipelineDebugName, shaderStageName, shader._debugName
			);
	}

	VkPipeline CTX::buildRayTracingPipelineImpl(VkPipelineLayout layout, RayTracingPipelineSpec& spec) {
		// sc_count is across all stages of a shader
		uint32_t sc_count = 0;
		while (sc_count < 16 && spec.specConstants[sc_count].size) {
			sc_count++;
		}

		// track number for later states
		uint32_t numShaderStages = 0;
		uint32_t numShaderGroups = 0;
		// track individual shader stages
		uint32_t idxMiss = 0;
		uint32_t numMissGroups = 0;
		uint32_t idxHit = 0;
		uint32_t numHitGroups = 0;
		uint32_t idxCallable = 0;
		uint32_t numCallableGroups = 0;

		// a single hit group can have up to 3 shader stages: source lvkweight said so
		const size_t kMaxRayTracingShaderStages =
			spec.rayGenShaders.size() +
				spec.missShaders.size() +
					spec.callableShader.size() +
						3 * spec.closestHitShaders.size();

		std::vector<VkPipelineShaderStageCreateInfo> createInfoShaderStages(kMaxRayTracingShaderStages);
		// pSpecializationInfo points into these, so they must outlive vkCreateRayTracingPipelinesKHR
		std::vector<std::unique_ptr<SpecializationInfoBundle>> specConstantsBundles;
		specConstantsBundles.reserve(kMaxRayTracingShaderStages);
		auto addStage = [this, sc_count, &spec, &numShaderStages, &createInfoShaderStages, &specConstantsBundles](VkShaderStageFlagBits stageFlag, const ShaderStage& shaderStage) -> uint32_t {
			if (shaderStage.handle.empty())
				return VK_SHADER_UNUSED_KHR;
			AllocatedShader* shader = _shaderPool.get(shaderStage.handle);
			ASSERT_MSG(shader, "A shader for raytracing pipeline: '{}' was destroyed or moved.", spec.debugName);
			ASSERT_MSG(shaderStage.entryPoint, "RayTracing shaders have no default entry point, one must be given for raytracing pipeline: '{}'.", spec.debugName);
			specConstantsBundles.push_back(BuildSpecializationInfoBundle(spec.specConstants, sc_count, shader->_specializationInfo.nameToID));
			const SpecializationInfoBundle* bundle = specConstantsBundles.back().get();
			createInfoShaderStages[numShaderStages] = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.stage = stageFlag,
				.module = shader->_vkShaderModule,
				.pName = shaderStage.entryPoint,
				.pSpecializationInfo = bundle ? &bundle->vkInfo : nullptr
			};
			return numShaderStages++;
		};


		const uint32_t kMaxShaderGroups =
			spec.rayGenShaders.size() +
				spec.missShaders.size() +
					spec.callableShader.size() +
						spec.closestHitShaders.size();
		// we can safely index into it
		std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups(kMaxShaderGroups);

		for (auto& raygenShader : spec.rayGenShaders) {
			if (auto shader = _shaderPool.get(raygenShader.handle)) {
				CheckSpecializationConstants(*shader, sc_count, "ray generatioon", spec.debugName, true);
				shaderGroups[numShaderGroups++] = {
					.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
					.generalShader = addStage(VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygenShader),
					.closestHitShader = VK_SHADER_UNUSED_KHR,
					.anyHitShader = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				};
			}
		}
		for (auto& missShader : spec.missShaders) {
			if (auto shader = _shaderPool.get(missShader.handle)) {
				CheckSpecializationConstants(*shader, sc_count, "miss", spec.debugName, true);
				numMissGroups++;
				shaderGroups[numShaderGroups++] = {
					.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
					.generalShader = addStage(VK_SHADER_STAGE_MISS_BIT_KHR, missShader),
					.closestHitShader = VK_SHADER_UNUSED_KHR,
					.anyHitShader = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				};
			}
		}
		for (auto& closestHitShader : spec.closestHitShaders) {
			if (auto shader = _shaderPool.get(closestHitShader.handle)) {
				CheckSpecializationConstants(*shader, sc_count, "closest hit", spec.debugName, true);
				numHitGroups++;
				shaderGroups[numShaderGroups++] = {
					.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader = VK_SHADER_UNUSED_KHR,
					.closestHitShader = addStage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closestHitShader),
					.anyHitShader = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				};
			}
		}
		for (auto& callableShader : spec.callableShader) {
			if (auto shader = _shaderPool.get(callableShader.handle)) {
				CheckSpecializationConstants(*shader, sc_count, "callable", spec.debugName, true);
				numCallableGroups++;
				shaderGroups[numShaderGroups++] = {
					.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
					.generalShader = addStage(VK_SHADER_STAGE_CALLABLE_BIT_KHR, callableShader),
					.closestHitShader = VK_SHADER_UNUSED_KHR,
					.anyHitShader = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				};
			}
		}

		const uint32_t maxDepth = _rayTracingProperties.maxRayRecursionDepth;
		const uint32_t depth = spec.maxRecursionDepth ? spec.maxRecursionDepth : 0;
		// only need
		constexpr VkDynamicState dynamicState = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
		const VkPipelineDynamicStateCreateInfo ciDynamicState = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 1,
			.pDynamicStates = &dynamicState
		};
		const VkRayTracingPipelineCreateInfoKHR pipeline_ci = {
			.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
			.pNext = nullptr,
			.flags = 0,
			.stageCount = numShaderStages,
			.pStages = createInfoShaderStages.data(),
			.groupCount = numShaderGroups,
			.pGroups = shaderGroups.data(),
			.maxPipelineRayRecursionDepth = depth ? std::min(depth, maxDepth) : maxDepth,
			.pDynamicState = spec.stackSize ? &ciDynamicState : nullptr,
			.layout = layout
		};
		VkPipeline vk_pipeline = VK_NULL_HANDLE;
		VK_CHECK(vkCreateRayTracingPipelinesKHR(_vkDevice, nullptr, nullptr, 1, &pipeline_ci, nullptr, &vk_pipeline));

		return vk_pipeline;
	}

	void CTX::resolveComputePipelineImpl(AllocatedComputePipeline& pipeline) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		// aliases
		ComputePipelineSpec& spec = pipeline._spec;
		AllocatedShader* shader = _shaderPool.get(spec.shader);

		// resolvement and assignment
		PipelineCoreData common = this->buildPipelineCommonDataExceptVkPipelineImpl(shader->_pipelineSignature);
		pipeline._shared.core = common;
		pipeline._shared.core._vkPipeline = this->buildComputePipelineImpl(common._vkPipelineLayout, spec);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline._shared.core._vkPipeline), pipeline.getDebugName().data());
		// good to go, maybe later you could return it
	}
	void CTX::resolveRayTracingPipelineImpl(AllocatedRayTracingPipeline& pipeline) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		// aliases
		RayTracingPipelineSpec& spec = pipeline._spec;

		// shader signature collection
		std::vector<PipelineLayoutSignature> pipeline_layout_signatures = {};
		for (const std::vector<ShaderStage>* stages : {&spec.rayGenShaders, &spec.missShaders, &spec.closestHitShaders, &spec.callableShader}) {
			for (const ShaderStage& stage : *stages) {
				if (!stage.valid())
					continue;
				AllocatedShader* shader = _shaderPool.get(stage.handle);
				ASSERT_MSG(shader, "A shader for raytracing pipeline: '{}' was destroyed or moved.", spec.debugName);
				pipeline_layout_signatures.push_back(shader->_pipelineSignature);
			}
		}
		const PipelineLayoutSignature merged_pl_signature = MergeSignatures(pipeline_layout_signatures);

		// resolvement and assignment
		PipelineCoreData common = this->buildPipelineCommonDataExceptVkPipelineImpl(merged_pl_signature);
		pipeline._shared.core = common;
		pipeline._shared.core._vkPipeline = this->buildRayTracingPipelineImpl(common._vkPipelineLayout, spec);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline._shared.core._vkPipeline), pipeline.getDebugName().data());
		buildShaderBindingTableImpl(pipeline);
	}

	void CTX::invalidateBindlessPipelineImpl(SharedPipelineInfo& shared) {
		PipelineCoreData& core = shared.core;
		if (core._vkPipeline == VK_NULL_HANDLE || core._vkBindlessDescriptorSet == VK_NULL_HANDLE)
			return;
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = core._vkPipeline]() { vkDestroyPipeline(device, pipeline, nullptr); }));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = core._vkPipelineLayout]() { vkDestroyPipelineLayout(device, layout, nullptr); }));
		core._vkPipeline = VK_NULL_HANDLE;
		core._vkPipelineLayout = VK_NULL_HANDLE;
		core._vkBindlessDescriptorSet = VK_NULL_HANDLE;
	}

	void CTX::buildShaderBindingTableImpl(AllocatedRayTracingPipeline& pipeline) {
		const RayTracingPipelineSpec& spec = pipeline._spec;
		// must mirror the group order and skip rule in buildRayTracingPipelineImpl
		const auto countLive = [this](const std::vector<ShaderStage>& stages) {
			uint32_t count = 0;
			for (const ShaderStage& stage: stages) {
				if (_shaderPool.get(stage.handle))
					count++;
			}
			return count;
		};
		const uint32_t numRaygen = countLive(spec.rayGenShaders);
		const uint32_t numMiss = countLive(spec.missShaders);
		const uint32_t numHit = countLive(spec.closestHitShaders);
		const uint32_t numCallable = countLive(spec.callableShader);
		const uint32_t numGroups = numRaygen + numMiss + numHit + numCallable;
		ASSERT_MSG(numRaygen > 0, "RayTracing pipeline '{}' needs at least one ray generation shader.", spec.debugName);

		const uint32_t handleSize = _rayTracingProperties.shaderGroupHandleSize;
		const uint32_t handleAlignment = _rayTracingProperties.shaderGroupHandleAlignment;
		const uint32_t baseAlignment = _rayTracingProperties.shaderGroupBaseAlignment;
		ASSERT_MSG(handleSize && handleAlignment && baseAlignment, "Ray tracing pipeline properties were never queried.");
		const VkDeviceSize handleStride = getAlignedAddress(handleSize, handleAlignment);
		const VkDeviceSize raygenSize = getAlignedAddress(handleStride, baseAlignment);
		const VkDeviceSize missSize = getAlignedAddress(numMiss * handleStride, baseAlignment);
		const VkDeviceSize hitSize = getAlignedAddress(numHit * handleStride, baseAlignment);
		const VkDeviceSize callableSize = getAlignedAddress(numCallable * handleStride, baseAlignment);

		std::vector<uint8_t> handles(static_cast<size_t>(numGroups) * handleSize);
		VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(_vkDevice, pipeline._shared.core._vkPipeline, 0, numGroups, handles.size(), handles.data()));

		std::vector<uint8_t> table(raygenSize + missSize + hitSize + callableSize + baseAlignment, 0);
		char buffer_name[128];
		snprintf(buffer_name, sizeof(buffer_name), "SBT - %s", spec.debugName);
		pipeline._sbtBuffer = this->createBuffer({
			.size = table.size(),
			.usage = BufferUsageBits_ShaderBindingTable,
			.storage = StorageType::HostVisible,
			.debugName = buffer_name
		});
		const VkDeviceAddress bufferAddress = gpuAddress(pipeline._sbtBuffer.handle());
		const VkDeviceAddress raygenAddress = getAlignedAddress(bufferAddress, baseAlignment);
		const VkDeviceSize baseOffset = raygenAddress - bufferAddress;

		uint32_t group = 0;
		const auto writeRegion = [&](VkDeviceSize regionOffset, uint32_t count) {
			for (uint32_t i = 0; i < count; i++, group++) {
				memcpy(table.data() + baseOffset + regionOffset + i * handleStride, handles.data() + static_cast<size_t>(group) * handleSize, handleSize);
			}
		};
		writeRegion(0, 1);
		group = numRaygen;
		writeRegion(raygenSize, numMiss);
		writeRegion(raygenSize + missSize, numHit);
		writeRegion(raygenSize + missSize + hitSize, numCallable);
		upload(pipeline._sbtBuffer.handle(), table.data(), table.size());

		pipeline._sbtEntryRayGen = VkStridedDeviceAddressRegionKHR{.deviceAddress = raygenAddress, .stride = raygenSize, .size = raygenSize};
		pipeline._sbtEntryMiss = numMiss ? VkStridedDeviceAddressRegionKHR{.deviceAddress = raygenAddress + raygenSize, .stride = handleStride, .size = missSize} : VkStridedDeviceAddressRegionKHR{};
		pipeline._sbtEntryHit = numHit ? VkStridedDeviceAddressRegionKHR{.deviceAddress = raygenAddress + raygenSize + missSize, .stride = handleStride, .size = hitSize} : VkStridedDeviceAddressRegionKHR{};
		pipeline._sbtEntryCallable = numCallable ? VkStridedDeviceAddressRegionKHR{.deviceAddress = raygenAddress + raygenSize + missSize + hitSize, .stride = handleStride, .size = callableSize} : VkStridedDeviceAddressRegionKHR{};
	}

	void CTX::resolveGraphicsPipelineImpl(AllocatedGraphicsPipeline& pipeline, uint32_t viewMask) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);

		GraphicsPipelineSpec& spec = pipeline._spec;

		// things the user defined previously
		GraphicsPipelineBuilder builder = {};
		builder.set_cull_mode(spec.cull);
		builder.set_polygon_mode(spec.polygon);
		builder.set_topology_mode(spec.topology);
		builder.set_multisampling_mode(spec.multisample);
		builder.set_blending_mode(spec.blend);
		builder.set_viewmask(viewMask);

		// things we gather in the middle of the renderpass (attachment formats)
		// looks at the current pass and reflects those formats in use
		ASSERT(_currentCommandBuffer._activePass);
		const CompiledPass& current_pass_info = *_currentCommandBuffer._activePass;
		std::vector<VkFormat> colorFormats;
		colorFormats.reserve(current_pass_info.colorAttachments.size());
		for (const AttachmentInfo& colorAttachment: current_pass_info.colorAttachments) {
			colorFormats.push_back(colorAttachment.imageFormat);
		}
		builder.set_color_formats(colorFormats);
		if (current_pass_info.depthAttachment.has_value()) {
			builder.set_depth_format(current_pass_info.depthAttachment->imageFormat);
		}
		// manually resolve the # of spec constants user gave
		uint32_t sc_count = 0;
		while (sc_count < 16 && spec.specConstants[sc_count].size) {
			sc_count++;
		}

		// shader setup & signature collection
		std::vector<PipelineLayoutSignature> pipeline_layout_signatures = {};

		ShaderStage& vertStage = spec.vertexShader;
		std::unique_ptr<SpecializationInfoBundle> vertSpecConstantsBundle;
		if (vertStage.valid()) {
			if (!vertStage.entryPoint) {
				vertStage.entryPoint = "vs_main";
			}
			AllocatedShader* shader = _shaderPool.get(vertStage.handle);
			ASSERT_MSG(shader, "The vertex shader for graphics pipeline: '{}' was destroyed or moved.", spec.debugName);
			CheckSpecializationConstants(*shader, sc_count, "vertex", spec.debugName, true);
			vertSpecConstantsBundle = BuildSpecializationInfoBundle(spec.specConstants, sc_count, shader->_specializationInfo.nameToID);
			builder.add_shader_module(shader->_vkShaderModule, VK_SHADER_STAGE_VERTEX_BIT, vertStage.entryPoint, &vertSpecConstantsBundle->vkInfo);
			pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		}
		ShaderStage& fragStage = spec.fragmentShader;
		std::unique_ptr<SpecializationInfoBundle> fragSpecConstantsBundle;
		if (fragStage.valid()) {
			if (!fragStage.entryPoint) {
				fragStage.entryPoint = "fs_main";
			}
			AllocatedShader* shader = _shaderPool.get(fragStage.handle);
			ASSERT_MSG(shader, "The fragment shader for graphics pipeline: '{}' was destroyed or moved.", spec.debugName);
			CheckSpecializationConstants(*shader, sc_count, "fragment", spec.debugName, true);
			fragSpecConstantsBundle = BuildSpecializationInfoBundle(spec.specConstants, sc_count, shader->_specializationInfo.nameToID);
			builder.add_shader_module(shader->_vkShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT, fragStage.entryPoint, &fragSpecConstantsBundle->vkInfo);
			pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		}
		// we dont support geometry shaders because my mac doesnt :)

		// ShaderStage& geometryStage = spec.geometryShader;
		// std::unique_ptr<SpecializationInfoBundle> geomSpecConstantsBundle;
		// if (geometryStage.valid()) {
		// 	if (!geometryStage.entryPoint) {
		// 		geometryStage.entryPoint = "gs_main";
		// 	}
		// 	AllocatedShader* shader = _shaderPool.get(geometryStage.handle);
		// 	if (shader->_specializationInfo.specializationConstants.size() > sc_count)
		// 		LOG_SYSTEM(LogType::Warning, "You have specialization constants used in the geometry shader '{}' that are not defined in the pipeline creation for '{}'!", shader->_debugName,
		// spec.debugName); 	geomSpecConstantsBundle = BuildSpecializationInfoBundle(spec.specConstants, sc_count, shader->_specializationInfo.nameToID);
		// 	builder.add_shader_module(shader->vkShaderModule, VK_SHADER_STAGE_GEOMETRY_BIT, geometryStage.entryPoint, &geomSpecConstantsBundle->vkInfo);
		// 	pipeline_layout_signatures.push_back(shader->_pipelineSignature);
		// }

		const PipelineLayoutSignature merged_pl_signature = MergeSignatures(pipeline_layout_signatures);

		PipelineCoreData common = this->buildPipelineCommonDataExceptVkPipelineImpl(merged_pl_signature);
		pipeline._shared.core = common;
		pipeline._shared.core._vkPipeline = builder.build(_vkDevice, common._vkPipelineLayout);
		// 0 represents the first compile of a pipeline
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline._shared.core._vkPipeline), pipeline.getDebugName().data());
		// good to go, maybe later you could return it
	}

	void CTX::switchShader(GraphicsPipeline& graphicsPipeline, Shader& newShader, ShaderStages stage) {
		AllocatedGraphicsPipeline* graphics_pipeline_obj = this->_graphicsPipelinePool.get(graphicsPipeline.handle());
		if (!graphics_pipeline_obj) {
			LOG_SYSTEM(LogType::Warning, "'switchShader' called on null/invalid graphics_pipeline!");
			return;
		}
		GraphicsPipelineSpec current_spec = graphics_pipeline_obj->_spec;
		PipelineCoreData& core = graphics_pipeline_obj->_shared.core;
		//VkShaderModule currentVkShaderModule = current_spec.vertexShader;

		// same thing as destroying the pipeline
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = core._vkPipeline]() { vkDestroyPipeline(device, pipeline, nullptr); }));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = core._vkPipelineLayout]() { vkDestroyPipelineLayout(device, layout, nullptr); }));

		core._vkPipeline = VK_NULL_HANDLE;
		core._vkPipelineLayout = VK_NULL_HANDLE;
		switch (stage) {
			case ShaderStages::Vertex: {
				graphics_pipeline_obj->_spec.vertexShader = newShader;
			} break;
			case ShaderStages::Fragment: {
				graphics_pipeline_obj->_spec.fragmentShader = newShader;
			} break;
			default: ASSERT_MSG(false, "Currently only ShaderStages:: Vertex & Fragment are supported!");
		}
	}

	Sampler CTX::createSampler(SamplerSpec spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		// creating sampler requires little work so we dont need an _Impl function for it
		VkSamplerCreateInfo info = {
		    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .pNext = nullptr,
		    .flags = 0,

		    .magFilter = toVulkan(spec.magFilter),
		    .minFilter = toVulkan(spec.minFilter),
		    .mipmapMode = toVulkan(spec.mipMap),
		    .addressModeU = toVulkan(spec.wrapU),
		    .addressModeV = toVulkan(spec.wrapV),
		    .addressModeW = toVulkan(spec.wrapW),

		    .mipLodBias = 0.f,
		    .anisotropyEnable = spec.anistrophic ? VK_TRUE : VK_FALSE,
		    .maxAnisotropy = static_cast<float>(spec.maxAnisotropic),

		    .compareEnable = spec.depthCompareEnabled ? VK_TRUE : VK_FALSE,
		    .compareOp = spec.depthCompareEnabled ? toVulkan(spec.depthCompareOp) : VK_COMPARE_OP_ALWAYS,

		    .minLod = static_cast<float>(spec.mipLodMin),
		    .maxLod = (spec.mipMap == SamplerMipMap::Disabled) ? static_cast<float>(spec.mipLodMin) : static_cast<float>(spec.mipLodMax),

		    .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		    .unnormalizedCoordinates = VK_FALSE,
		};
		auto a = this->getPhysicalDeviceProperties10();

		if (spec.maxAnisotropic > 1) {
			const float maxSamplerAnisotropy = this->getPhysicalDeviceProperties10().limits.maxSamplerAnisotropy;
			const bool isAnisotropicFilteringSupported = maxSamplerAnisotropy > 1;
			ASSERT_MSG(isAnisotropicFilteringSupported, "Anisotropic filtering is not supported by the device.");
			info.anisotropyEnable = spec.anistrophic ? VK_TRUE : VK_FALSE;
			// fixme: not sure why maxAnisotropic is of type uint_
			if (maxSamplerAnisotropy < static_cast<float>(spec.maxAnisotropic)) {
				LOG_SYSTEM(LogType::Warning, "Supplied sampler anisotropic value greater than max supported by the device, setting to {}", static_cast<double>(maxSamplerAnisotropy));
			}
			info.maxAnisotropy = std::min(static_cast<float>(maxSamplerAnisotropy), static_cast<float>(spec.maxAnisotropic));
		}

		AllocatedSampler obj = {};
		vkCreateSampler(_vkDevice, &info, nullptr, &obj._vkSampler);
		snprintf(obj._debugName, sizeof(obj._debugName), "%s", spec.debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(obj._vkSampler), obj.getDebugName().data());
		SamplerHandle handle = _samplerPool.create(std::move(obj));
		writeSamplerDescriptorsImpl(handle.index(), 1);
		return {this, handle};
	}
	Buffer CTX::createBuffer(BufferSpec spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		VkBufferUsageFlags usage_flags = (spec.storage == StorageType::Device) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0;

		if (spec.usage & BufferUsageBits::BufferUsageBits_Index)
			usage_flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_Storage)
			usage_flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_Indirect)
			usage_flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_AccelerationStructureInputReadOnly)
			usage_flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_AccelerationStructureStorage)
			usage_flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		if (spec.usage & BufferUsageBits::BufferUsageBits_ShaderBindingTable)
			usage_flags |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		ASSERT_MSG(usage_flags, "Invalid buffer creation specification!");
		const VkMemoryPropertyFlags mem_flags = StorageTypeToVkMemoryPropertyFlags(spec.storage);

		AllocatedBuffer obj = this->createBufferImpl(spec.size, usage_flags, mem_flags);
		snprintf(obj._debugName, sizeof(obj._debugName), "%s", spec.debugName);
		vmaSetAllocationName(_vmaAllocator, obj._vmaAllocation, obj._debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(obj._vkBuffer), obj.getDebugName().data());
		BufferHandle handle = _bufferPool.create(std::move(obj));
		if (spec.initialData) {
			upload(handle, spec.initialData, spec.size);
		}
		return {this, handle};
	}
	AllocatedBuffer CTX::createBufferImpl(VkDeviceSize bufferSize, VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memFlags) {
		ASSERT_MSG(bufferSize > 0, "Buffer size needs to be greater than 0!");

		AllocatedBuffer obj = {};
		obj._bufferSize = bufferSize;
		obj._vkUsageFlags = usageFlags;
		obj._vkMemoryPropertyFlags = memFlags;

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
		vmaAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		VmaAllocationInfo allocInfo = {};
		VK_CHECK(vmaCreateBufferWithAlignment(_vmaAllocator, &buffer_ci, &vmaAllocInfo, 16, &obj._vkBuffer, &obj._vmaAllocation, &allocInfo));
		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			obj._mappedPtr = allocInfo.pMappedData;
			VkMemoryPropertyFlags actualFlags = 0;
			vmaGetAllocationMemoryProperties(_vmaAllocator, obj._vmaAllocation, &actualFlags);
			obj._isCoherentMemory = actualFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		}

		ASSERT_MSG(obj._vkBuffer != VK_NULL_HANDLE, "VkBuffer is VK_NULL_HANDLE after creation!");

		// shader access
		if (usageFlags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
			const VkBufferDeviceAddressInfo buffer_device_ai = {
			    .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			    .buffer = obj._vkBuffer,
			};
			obj._vkDeviceAddress = vkGetBufferDeviceAddress(_vkDevice, &buffer_device_ai);
			ASSERT(obj._vkDeviceAddress);
		}
		return obj;
	}

	void CTX::resizeBuffer(BufferHandle handle, size_t newSize) {
		MYTH_PROFILER_FUNCTION();
		AllocatedBuffer* buffer = _bufferPool.get(handle);
		if (!buffer) {
			LOG_SYSTEM(LogType::Warning, "'resizeBuffer' called on invalid handle!");
			return;
		}
		if (newSize <= buffer->_bufferSize) {
			LOG_SYSTEM(LogType::Warning, "'resizeBuffer' called with a smaller size then it already has has.");
		}

		deferTask(std::packaged_task<void()>([vma = _vmaAllocator, vkBuf = buffer->_vkBuffer, allocation = buffer->_vmaAllocation]() {
			vmaDestroyBuffer(vma, vkBuf, allocation);
		}));

		AllocatedBuffer newBuf = this->createBufferImpl(newSize, buffer->_vkUsageFlags, buffer->_vkMemoryPropertyFlags);
		vmaSetAllocationName(_vmaAllocator, newBuf._vmaAllocation, buffer->_debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(newBuf._vkBuffer), buffer->getDebugName().data());

		buffer->_vkBuffer = newBuf._vkBuffer;
		buffer->_vmaAllocation = newBuf._vmaAllocation;
		buffer->_bufferSize = newBuf._bufferSize;
		buffer->_vkDeviceAddress = newBuf._vkDeviceAddress;
		buffer->_mappedPtr = newBuf._mappedPtr;
		buffer->_isCoherentMemory = newBuf._isCoherentMemory;
		// we dont need an epoch (framegraph doesnt need to recompile)
	}

	void CTX::resizeTexture(TextureHandle handle, Dimensions newDimensions) {
		MYTH_PROFILER_FUNCTION();
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot resize a texture while a CommandBuffer is open, already recorded commands reference the old image.");
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image) {
			LOG_SYSTEM(LogType::Warning, "'resizeTexture' called on invalid handle!");
			return;
		}
		// copies destroy for Texture logic
		deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageView]() { vkDestroyImageView(device, imageView, nullptr); }));
		if (image->_vkImageViewStorage) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageViewStorage]() { vkDestroyImageView(device, imageView, nullptr); }));
		}
		if (image->_isOwning) {
			if (image->_mappedPtr) {
				vmaUnmapMemory(_vmaAllocator, image->_vmaAllocation);
			}
			deferTask(std::packaged_task<void()>([vma = _vmaAllocator, image = image->_vkImage, allocation = image->_vmaAllocation]() { vmaDestroyImage(vma, image, allocation); }));
		}

		VkImageCreateFlags createFlags = 0;
		// resolve createFlags without having it stored
		if (image->_vkImageViewType == VK_IMAGE_VIEW_TYPE_CUBE || image->_vkImageViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
			createFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		}

		const VkExtent3D extent3D = {newDimensions.width, newDimensions.height, newDimensions.depth};
		AllocatedTexture newImage = this->createTextureImpl(
		        image->_vkUsageFlags,
		        image->_vkMemoryPropertyFlags,
		        extent3D,
		        image->_vkFormat,
		        image->_vkImageType,
		        image->_vkImageViewType,
		        image->_numLevels,
		        image->_numLayers,
		        image->getVkSampleCountBits(),
		        image->_vkComponentMappings,
		        createFlags
		);
		vmaSetAllocationName(_vmaAllocator, newImage._vmaAllocation, newImage._debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(newImage._vkImage), image->getDebugName().data());
#ifdef MYTH_ENABLED_IMGUI
		if (_imguiPlugin.isEnabled()) {
			_imguiPlugin.removeImageDescriptor(handle);
		}
#endif

		// update extent!
		image->_vkExtent = extent3D;

		// new opaque objects
		image->_vkImage = newImage._vkImage;
		image->_vkImageView = newImage._vkImageView;
		image->_vkImageViewStorage = newImage._vkImageViewStorage;
		image->_vmaAllocation = newImage._vmaAllocation;
		// reset of state
		image->setLayout(image->getWholeRange(), VK_IMAGE_LAYOUT_UNDEFINED);
		image->_mappedPtr = newImage._mappedPtr;
		++_resourceEpoch;
		// the slot is rewritten in place, pending frames may still read the old view through it
		_immGraphics->wait(_immGraphics->getLastSubmitHandle());
		writeTextureDescriptorsImpl(handle.index(), 1);
	}

	Texture CTX::createTexture(TextureSpec spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		ASSERT_MSG(spec.usage, "The usage field for TextureSpec is required, however '{}' was not given one!", spec.debugName);
		ASSERT_MSG(!(spec.generateMipmaps && spec.initialData == nullptr), "'generateMipMaps' can only be true when 'initialData' is non-null!");
		// resolve usage flags
		VkImageUsageFlags usage_flags = (spec.storage == StorageType::Device) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;

		if (spec.usage & TextureUsageBits::TextureUsageBits_Sampled) {
			ASSERT_MSG(!vkutil::IsFormatDepthAndStencil(spec.format), "VkFormat cannot have both depth & stencil components and be used for sampling! Occurs because of texture '{}'", spec.debugName);
			usage_flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (spec.usage & TextureUsageBits::TextureUsageBits_Storage) {
			ASSERT_MSG(spec.samples == SampleCount::X1, "Storage images cannot be multisampled!");
			usage_flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (spec.usage & TextureUsageBits::TextureUsageBits_Attachment) {
			usage_flags |= vkutil::IsFormatDepthOrStencil(spec.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (spec.storage == StorageType::Memoryless) {
				usage_flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
			}
		}
		if (spec.storage != StorageType::Memoryless) {
			// for now, always set this flag so we can read it back
			usage_flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (spec.numMipLevels < 1) {
			LOG_SYSTEM(LogType::Warning, "The number of mip levels specified must be greater than 0!");
			spec.numMipLevels = 1;
		}
		const uint32_t _numLevels = spec.numMipLevels;
		ASSERT_MSG(usage_flags != 0, "Invalid usage flags for texture creation!");
		ASSERT_MSG(spec.type == TextureType::Type_2D || spec.type == TextureType::Type_3D || spec.type == TextureType::Type_Cube, "Only 2D, 3D and Cube textures are supported.");

		// resolve sample count
		const VkSampleCountFlagBits sample_bits = toVulkan(spec.samples);
		// resolve memory flags
		const VkMemoryPropertyFlags mem_flags = StorageTypeToVkMemoryPropertyFlags(spec.storage);
		// resolve extent3D
		const VkExtent3D extent3D = {spec.dimension.width, spec.dimension.height, spec.dimension.depth};

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
				_imageviewtype = _numLayers > 1 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
				_numLayers *= 6;
				_imageCreateFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
				break;
			default:
				assert(false);
		}
		const VkComponentMapping component_mappings = spec.components.toVkComponentMapping();
		AllocatedTexture obj = createTextureImpl(usage_flags, mem_flags, extent3D, spec.format, _imagetype, _imageviewtype, _numLevels, _numLayers, sample_bits, component_mappings, _imageCreateFlags);
		// members that are only needed for recreation
		obj._vkMemoryPropertyFlags = mem_flags;
		obj._vkImageViewType = _imageviewtype;
		snprintf(obj._debugName, sizeof(obj._debugName), "%s", spec.debugName);
		vmaSetAllocationName(_vmaAllocator, obj._vmaAllocation, obj._debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(obj._vkImage), obj.getDebugName().data());
		char d[32];
		snprintf(d, sizeof(d), "%s - Default View", spec.debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(obj._vkImageView), d);
		TextureHandle handle = _texturePool.create(std::move(obj));
		writeTextureDescriptorsImpl(handle.index(), 1);
		// if we have some data we want to upload, do that
		if (spec.initialData) {
			// enforces that the number of miplevels we want to upload will be accomadated by the texture
			ASSERT(spec.dataNumMipLevels <= spec.numMipLevels);
			ASSERT(spec.type == TextureType::Type_2D || spec.type == TextureType::Type_Cube);
			const TexRange range = {.dimensions = extent3D, .numLayers = static_cast<uint32_t>((spec.type == TextureType::Type_Cube) ? 6 : 1), .numMipLevels = spec.dataNumMipLevels};
			this->upload(handle, spec.initialData, range);
			if (spec.generateMipmaps) {
				this->generateMipmaps(handle);
			}
		}
		return {this, handle};
	}
	// todo: review this, as it seems alot of the data can simply be read straight from the copy instead of being resolved again
	// https://github.com/corporateshark/lightweightvk/blob/master/lvk/vulkan/VulkanClasses.cpp#L4351
	TextureHandle CTX::createTextureViewImpl(TextureHandle handle, TextureViewSpec spec) {
		if (!handle.valid()) {
			LOG_SYSTEM(LogType::Warning, "Texture handle is empty!");
			return {};
		}
		// make a copy of the allocated texture object
		AllocatedTexture copied_obj = *this->_texturePool.get(handle);
		// view is its own resource, don't inherit the parent's cached views
		copied_obj._additionalViews.clear();
		ASSERT_MSG(copied_obj._numLevels > spec.mipLevel, "baseMipLevel must be less then mipLevels of the original texture!");
		if (spec.numMipLevels != VK_REMAINING_MIP_LEVELS) {
			ASSERT_MSG(copied_obj._numLevels >= (spec.mipLevel + spec.numMipLevels), "Requested mipLevel+numMipLevels exceeds the number of mipLevels of the original texture!");
		}
		// very important!
		copied_obj._isOwning = false;
		const VkImageAspectFlags texture_aspect_flags = copied_obj.getImageAspectFlags();
		// lose all data associated with original image
		memset(&copied_obj._vkImageViewStorage, 0, sizeof(copied_obj._vkImageViewStorage));

		const VkComponentMapping component_mappings = spec.components.toVkComponentMapping();
		ASSERT_MSG(vkutil::GetNumImagePlanes(copied_obj._vkFormat) == 1, "Unsupported multiplanar image.");

		if (spec.type == VK_IMAGE_VIEW_TYPE_MAX_ENUM)
			spec.type = copied_obj._vkImageViewType;

		VkImageViewCreateInfo image_view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .pNext = nullptr,
		    .image = copied_obj._vkImage,
		    .viewType = spec.type,
		    .format = copied_obj._vkFormat,
		    .components = component_mappings,
		    .subresourceRange = {.aspectMask = texture_aspect_flags, .baseMipLevel = spec.mipLevel, .levelCount = spec.numMipLevels, .baseArrayLayer = spec.layer, .layerCount = spec.numLayers},
		};
		VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &copied_obj._vkImageView));
		if (copied_obj._vkUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) {
			if (!vkutil::IsComponentMappingAnIdentity(component_mappings)) {
				image_view_ci.components = {};
				VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &copied_obj._vkImageViewStorage));
			}
		}

		snprintf(copied_obj._debugName, sizeof(copied_obj._debugName), "%s", spec.debugName);
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(copied_obj._vkImageView), copied_obj.getDebugName().data());
		TextureHandle new_handle = this->_texturePool.create(std::move(copied_obj));
		writeTextureDescriptorsImpl(new_handle.index(), 1);
		return {new_handle};
	}
	AllocatedTexture CTX::createTextureImpl(
	        VkImageUsageFlags usageFlags,
	        VkMemoryPropertyFlags memFlags,
	        VkExtent3D extent3D,
	        VkFormat format,
	        VkImageType imageType,
	        VkImageViewType imageViewType,
	        uint32_t numLevels,
	        uint32_t numLayers,
	        VkSampleCountFlagBits sampleCountFlagBits,
	        VkComponentMapping componentMapping,
	        VkImageCreateFlags createFlags
	) {
		ASSERT_MSG(numLevels > 0, "The texture must contain at least one mip-level!");
		ASSERT_MSG(numLayers > 0, "The texture must contain at least one layer!");
		ASSERT_MSG(extent3D.width > 0, "The texture must have a width greater than 0!");
		ASSERT_MSG(extent3D.height > 0, "The texture must have a height greater than 0!");
		ASSERT_MSG(extent3D.depth > 0, "The texture must have a depth greater than 0!");


		VkImageCreateInfo image_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .pNext = nullptr,

		    .flags = createFlags,
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

		VmaAllocationCreateInfo allocation_ci = {};
		allocation_ci.usage = (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_AUTO;

		AllocatedTexture obj = {};
		VK_CHECK(vmaCreateImage(_vmaAllocator, &image_ci, &allocation_ci, &obj._vkImage, &obj._vmaAllocation, nullptr));
		obj._vkExtent = extent3D;
		obj._vkUsageFlags = usageFlags;
		obj._sampleCount = sampleCountFlagBits;
		obj._vkImageType = imageType;
		obj._vkImageViewType = imageViewType;
		obj._vkFormat = format;
		obj._numLevels = numLevels;
		obj._numLayers = numLayers;
		obj._recordedLayout = SubresourceMap<VkImageLayout>{numLevels, numLayers, VK_IMAGE_LAYOUT_UNDEFINED};
		VkFormatProperties2 format_props2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
		vkGetPhysicalDeviceFormatProperties2(_vkPhysicalDevice, obj._vkFormat, &format_props2);
		obj._vkFormatProperties = format_props2.formatProperties;

		const VkPhysicalDeviceImageFormatInfo2 info = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = obj.getFormat(), .type = obj.getType(), .usage = obj._vkUsageFlags, .flags = createFlags
		};
		VkImageFormatProperties2 props = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		};
		VK_CHECK(vkGetPhysicalDeviceImageFormatProperties2(_vkPhysicalDevice, &info, &props));
		ASSERT_MSG(
		        props.imageFormatProperties.maxMipLevels >= obj._numLevels,
		        "Requested image exceeds maxMipLevels, requested '{}', cannot be greater than '{}'",
		        obj._numLevels,
		        props.imageFormatProperties.maxMipLevels
		);


		// if memory is manually managed on host
		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			VK_CHECK(vmaMapMemory(_vmaAllocator, obj._vmaAllocation, &obj._mappedPtr));
		}
		// create image views
		const VkImageAspectFlags aspectMask = vkutil::AspectMaskFromFormat(obj._vkFormat);
		VkImageViewCreateInfo image_view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .flags = 0,
		    .image = obj._vkImage,
		    .viewType = obj._vkImageViewType,
		    .format = format,
		    .components = componentMapping,
		    .subresourceRange = {.aspectMask = aspectMask, .baseMipLevel = 0, .levelCount = numLevels, .baseArrayLayer = 0, .layerCount = numLayers},
		};
		VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &obj._vkImageView));
		if (obj._vkUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) {
			if (!vkutil::IsComponentMappingAnIdentity(componentMapping)) {
				image_view_ci.components = {};
				VK_CHECK(vkCreateImageView(_vkDevice, &image_view_ci, nullptr, &obj._vkImageViewStorage));
			}
		}
		return obj;
	}

	GraphicsPipeline CTX::createGraphicsPipeline(const GraphicsPipelineSpec& spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		// all creating a pipeline does is assign the spec to it
		// actual construction was done upon first use (lazily)
		// now its done when compile is called && has been by a dryRun,
		// if that fails then we do compile on first actual use
		AllocatedGraphicsPipeline obj = {};
		obj._spec = spec;
		snprintf(obj._shared.debugName, sizeof(obj._shared.debugName), "%s", spec.debugName);
		GraphicsPipelineHandle handle = _graphicsPipelinePool.create(std::move(obj));
		return {this, handle};
	}
	ComputePipeline CTX::createComputePipeline(const ComputePipelineSpec& spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		AllocatedComputePipeline obj = {};
		obj._spec = spec;
		snprintf(obj._shared.debugName, sizeof(obj._shared.debugName), "%s", spec.debugName);
		ComputePipelineHandle handle = _computePipelinePool.create(std::move(obj));
		return {this, handle};
	}
	// fixme: ensure correctness
	RayTracingPipeline CTX::createRayTracingPipeline(const RayTracingPipelineSpec& spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		AllocatedRayTracingPipeline obj = {};
		obj._spec = spec;
		snprintf(obj._shared.debugName, sizeof(obj._shared.debugName), "%s", spec.debugName);
		RayTracingPipelineHandle handle = _rayTracingPipelinePool.create(std::move(obj));
		return { this, handle };
	}

	Shader CTX::createShader(const ShaderSpec& spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
		// lazily create slang session & slang global session
		if (!_slangCompiler.sessionExists()) {
			_slangCompiler.create();
		}

		CompileResult compile_result;
		if (std::holds_alternative<std::filesystem::path>(spec.source)) {
			const auto& filepath = std::get<std::filesystem::path>(spec.source);
			ASSERT_MSG(std::filesystem::exists(filepath), "Shader filepath '{}' does not exist.", filepath.string());
			compile_result = _slangCompiler.compileSlangFromDisk(filepath);
		} else if (std::holds_alternative<std::string>(spec.source)) {
			const auto& code = std::get<std::string>(spec.source);
			ASSERT_MSG(!code.empty(), "Shader source code is empty.", code);
			compile_result = _slangCompiler.compileSlangFromSourceString(code);
		}
		ASSERT(compile_result.getSpirvCode());
		// TODO: this is some of the worst code i have ever written in my life, i am so sorry future me who will come back here and have to clean it
		// thank you past me you were right but now it should be alot better
		AllocatedShader obj{};
		// _debugName
		snprintf(obj._debugName, sizeof(obj._debugName), "%s", spec.debugName);

		auto* code = const_cast<uint32_t*>(compile_result.getSpirvCode());
		size_t size = compile_result.getSpirvSize();
		// we have to do this cause bmillsNV likes to have unpredictability in his codebase
		// https://github.com/shader-slang/slang/issues/9338
		PatchSpecConstants(code, size);

		// always reflect spirv AFTER we make changes to it
		ReflectionResult reflection_result = ReflectSPIRV(code, size);
		obj._pipelineSignature = reflection_result.pipelineLayoutSignature;
		obj._descriptorSets = std::move(reflection_result.retrievedDescriptorSets);
		obj._pushConstants = std::move(reflection_result.retrivedPushConstants);
		// specialization constants need to be stored until they are used in resolving a pipeline
		obj._specializationInfo = std::move(reflection_result.specializationInfo);

		// _vkShaderModule
		VkShaderModuleCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pNext = nullptr};
		create_info.flags = 0;
		create_info.pCode = code;
		create_info.codeSize = size;
		VkShaderModule shaderModule = VK_NULL_HANDLE;
		// after vkCreateShaderModule we no longer need CompileResult btw
		VK_CHECK(vkCreateShaderModule(_vkDevice, &create_info, nullptr, &shaderModule));
		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(shaderModule), obj.getDebugName().data());
		obj._vkShaderModule = shaderModule;

		ShaderHandle handle = _shaderPool.create(std::move(obj));
		return {this, handle};
	}

	// ReSharper disable once CppNotAllPathsReturnValue
	VkGeometryTypeKHR GeometryTypeToVk(AccelerationStructureSpec::GeometryType geomType) {
		switch (geomType) {
			case AccelerationStructureSpec::GeometryType::Triangle:
				return VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			case AccelerationStructureSpec::GeometryType::AABB:
				return VK_GEOMETRY_TYPE_AABBS_KHR;
			case AccelerationStructureSpec::GeometryType::Instances:
				return VK_GEOMETRY_TYPE_INSTANCES_KHR;
			case AccelerationStructureSpec::GeometryType::INVALID:
				ASSERT_MSG(false, "You passed in AccelerationStructureSpec::GeometryType::INVALID, whice is, per the name, INVALID, please pick an actual geometry type.");
		}
	}
	void CTX::getBLASBuildInfo(const BottomLevelBuildDesc& desc,
		VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo,
		VkAccelerationStructureGeometryKHR& outGeometry) {

		outGeometry = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
			.geometry = {
				.triangles = {
					VkAccelerationStructureGeometryTrianglesDataKHR{
						.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
						.vertexFormat = desc.geometry.vertexFormat,
						.vertexData = { .deviceAddress = gpuAddress(desc.geometry.vertexBuffer) },
						.vertexStride = desc.geometry.vertexStride,
						.maxVertex = desc.geometry.numVertices,
						.indexType = desc.geometry.indexType,
						.indexData = { .deviceAddress = gpuAddress(desc.geometry.indexBuffer) },
						.transformData = { .deviceAddress = desc.geometry.transformBuffer.has_value() ? gpuAddress(desc.geometry.transformBuffer.value()) : 0 }
					}
				}
			},
			.flags = desc.geometryFlags
		};
		const VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			.flags = desc.buildFlags,
			.geometryCount = 1,
			.pGeometries = &outGeometry,
		};
		outBuildSizesInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
			.pNext = nullptr
		};
		vkGetAccelerationStructureBuildSizesKHR(_vkDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, &desc.maxPrimitiveCount, &outBuildSizesInfo);
		const uint32_t alignment = _accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
		outBuildSizesInfo.accelerationStructureSize += alignment;
		outBuildSizesInfo.updateScratchSize += alignment;
		outBuildSizesInfo.buildScratchSize += alignment;
	}
	void CTX::getTLASBuildInfo(const TopLevelBuildDesc& desc,
		VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo,
		VkAccelerationStructureGeometryKHR& outGeometry) {

		outGeometry = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
			.geometry = {
				.instances = {
					.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
					.arrayOfPointers = VK_FALSE,
					.data = {
						.deviceAddress = _bufferPool.get(desc.instancesBuffer)->_vkDeviceAddress
					}
				}
			},
			.flags = desc.geometryFlags,
		};
		const VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
			.flags = desc.buildFlags,
			.geometryCount = 1,
			.pGeometries = &outGeometry,
		};

		outBuildSizesInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
			.pNext = nullptr
		};
		vkGetAccelerationStructureBuildSizesKHR(_vkDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, &desc.maxPrimitiveCount, &outBuildSizesInfo);
		const uint32_t alignment = _accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
		outBuildSizesInfo.accelerationStructureSize += alignment;
		outBuildSizesInfo.updateScratchSize += alignment;
		outBuildSizesInfo.buildScratchSize += alignment;

	}


	AllocatedAccelerationStructure CTX::createTLASImpl(const AccelerationStructureSpec& spec) {
		ASSERT_MSG(spec.type == AccelerationStructureSpec::Type::TLAS, "Really why are you calling this (createTLAS) when the type isnt even TLAS");
		ASSERT(spec.instancesBuffer.valid());
		ASSERT(spec.buildRange.primitiveCount);
		ASSERT(spec.geometryType == AccelerationStructureSpec::GeometryType::Instances);

		AllocatedAccelerationStructure obj {};
		obj._isTLAS = true;

		VkAccelerationStructureBuildSizesInfoKHR retrievedBuildSizesInfo;
		VkAccelerationStructureGeometryKHR retrievedBuildGeometryInfo;
		// get data abt our requested AS that we use everywhere
		getTLASBuildInfo({
			.instancesBuffer = spec.instancesBuffer,
			.geometryFlags = spec.geometryFlags,
			.maxPrimitiveCount = spec.buildRange.primitiveCount,
			.buildFlags = spec.buildFlags
		}, retrievedBuildSizesInfo, retrievedBuildGeometryInfo);

		// buffer
		{
			char buffer_name[128];
			snprintf(buffer_name, sizeof(buffer_name), "Buffer - %s", spec.debugName);
			obj._buffer = this->createBuffer({
				.size = retrievedBuildSizesInfo.accelerationStructureSize,
				.usage = BufferUsageBits_AccelerationStructureStorage,
				.storage = StorageType::Device,
				.debugName = buffer_name
			});
		}
		// scratch buffer
		{
			char buffer_name[128];
			snprintf(buffer_name, sizeof(buffer_name), "Scratch Buffer - %s", spec.debugName);
			obj._scratchBuffer = this->createBuffer({
				.size = retrievedBuildSizesInfo.buildScratchSize,
				.usage = BufferUsageBits_Storage,
				.storage = StorageType::Device,
				.debugName = buffer_name
			});
		}
		const VkAccelerationStructureCreateInfoKHR create_info = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.pNext = nullptr,
			.createFlags = 0,
			.buffer = _bufferPool.get(obj._buffer.handle())->_vkBuffer,
			.size = retrievedBuildSizesInfo.accelerationStructureSize,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		};
		// create acceleration structure
		VkAccelerationStructureKHR vk_acceleration_structure = VK_NULL_HANDLE;
		VK_CHECK(vkCreateAccelerationStructureKHR(_vkDevice, &create_info, nullptr, &vk_acceleration_structure));
		obj._vkAccelerationStructure = vk_acceleration_structure;

		// build acceleration structure
		const VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
			.flags = spec.buildFlags,
			.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
			.dstAccelerationStructure = vk_acceleration_structure,
			.geometryCount = 1,
			.pGeometries = &retrievedBuildGeometryInfo,
			.scratchData = { .deviceAddress =
				getAlignedAddress(gpuAddress(obj._scratchBuffer.handle()), _accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment) }
		};

		const VkAccelerationStructureBuildRangeInfoKHR accelerationBuildStructureRangeInfo = {
			.primitiveCount = spec.buildRange.primitiveCount,
			.primitiveOffset = spec.buildRange.primitiveOffset,
			.firstVertex = spec.buildRange.firstVertex,
			.transformOffset = spec.buildRange.transformOffset
		};
		// just stuff it into this ig
		const VkAccelerationStructureBuildRangeInfoKHR* accelerationBuildStructureRangeInfos = { &accelerationBuildStructureRangeInfo };
		CommandBuffer& cmd = acquireCommand(CommandBuffer::Type::General);
		vkCmdBuildAccelerationStructuresKHR(cmd._wrapper->_cmdBuf, 1, &buildGeometryInfo, &accelerationBuildStructureRangeInfos);
		_immGraphics->wait(submitCommand(cmd));

		// retrieve data abt acceleration structure: device address
		const VkAccelerationStructureDeviceAddressInfoKHR info = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
			.accelerationStructure = vk_acceleration_structure,
		};
		obj._vkDeviceAddress = vkGetAccelerationStructureDeviceAddressKHR(_vkDevice, &info);
		return obj;
	}

	AllocatedAccelerationStructure CTX::createBLASImpl(const AccelerationStructureSpec& spec) {
		ASSERT_MSG(spec.type == AccelerationStructureSpec::Type::BLAS, "Really why are you calling this (createBLAS) when the type isnt even BLAS");
		ASSERT(spec.geometryType == AccelerationStructureSpec::GeometryType::Triangle);

		AllocatedAccelerationStructure obj {};

		// complaints here should also be applied to above for VkTLAS
		// todo: reuse the geometry struct we have this is just rebuilding a previous step

		VkAccelerationStructureBuildSizesInfoKHR retrievedBuildSizesInfo;
		VkAccelerationStructureGeometryKHR retrievedBuildGeometryInfo;
		getBLASBuildInfo({
			.geometryType = GeometryTypeToVk(spec.geometryType),
			.geometry = AccelerationStructureGeometry{
				.vertexBuffer = spec.vertexBuffer,
				.vertexStride = spec.vertexStride,
				.vertexFormat = spec.vertexFormat,
				.numVertices = spec.numVertices,
				.indexBuffer = spec.indexBuffer,
				.indexType = spec.indexType,
				.transformBuffer = spec.transformBuffer,
			},
			.geometryFlags = spec.geometryFlags,
			.maxPrimitiveCount = spec.buildRange.primitiveCount,
			.buildFlags = spec.buildFlags,
		}, retrievedBuildSizesInfo, retrievedBuildGeometryInfo);
		//const auto sizes = getBLASBuildInfo({
		//	.geometryType = GeometryTypeToVk(spec.geometryType),
		//	.geometry = AccelerationStructureGeometry{
		//		.vertexBuffer = spec.vertexBuffer,
		//		.vertexStride = spec.vertexStride,
		//		.vertexFormat = spec.vertexFormat,
		//		.numVertices = spec.numVertices,
		//		.indexBuffer = spec.indexBuffer,
		//		.indexType = spec.indexType,
		//		.transformBuffer = spec.transformBuffer,
		//	},
		//	.geometryFlags = spec.geometryFlags
		//});

		// buffer
		{
			char buffer_name[128];
			snprintf(buffer_name, sizeof(buffer_name), "Buffer - %s", spec.debugName);
			obj._buffer = this->createBuffer({
				.size = retrievedBuildSizesInfo.accelerationStructureSize,
				.usage = BufferUsageBits_AccelerationStructureStorage,
				.storage = StorageType::Device,
				.debugName = buffer_name
			});
		}
		// scratch buffer
		{
			char buffer_name[128];
			snprintf(buffer_name, sizeof(buffer_name), "Scratch Buffer - %s", spec.debugName);
			obj._scratchBuffer = this->createBuffer({
				.size = std::max(retrievedBuildSizesInfo.buildScratchSize, retrievedBuildSizesInfo.updateScratchSize),
				.usage = BufferUsageBits_Storage,
				.storage = StorageType::Device,
				.debugName = buffer_name
			});
		}
		const VkAccelerationStructureCreateInfoKHR create_info = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.pNext = nullptr,
			.createFlags = 0,
			.buffer = _bufferPool.get(obj._buffer.handle())->_vkBuffer,
			.size = retrievedBuildSizesInfo.accelerationStructureSize,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		};

		VkAccelerationStructureKHR vk_acceleration_structure = VK_NULL_HANDLE;
		VK_CHECK(vkCreateAccelerationStructureKHR(_vkDevice, &create_info, nullptr, &vk_acceleration_structure));
		obj._vkAccelerationStructure = vk_acceleration_structure;

		// build acceleration structure
		const VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			.flags = spec.buildFlags,
			.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
			.dstAccelerationStructure = vk_acceleration_structure,
			.geometryCount = 1,
			.pGeometries = &retrievedBuildGeometryInfo,
			.scratchData = { .deviceAddress =
				getAlignedAddress(gpuAddress(obj._scratchBuffer.handle()), _accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment) }
		};

		const VkAccelerationStructureBuildRangeInfoKHR accelerationBuildStructureRangeInfo = {
			.primitiveCount = spec.buildRange.primitiveCount,
			.primitiveOffset = spec.buildRange.primitiveOffset,
			.firstVertex = spec.buildRange.firstVertex,
			.transformOffset = spec.buildRange.transformOffset
		};

		const VkAccelerationStructureBuildRangeInfoKHR* accelerationBuildStructureRangeInfos = { &accelerationBuildStructureRangeInfo };
		CommandBuffer& cmd = acquireCommand(CommandBuffer::Type::General);
		vkCmdBuildAccelerationStructuresKHR(cmd._wrapper->_cmdBuf, 1, &buildGeometryInfo, &accelerationBuildStructureRangeInfos);
		_immGraphics->wait(submitCommand(cmd));

		// retrieve data abt acceleration structure: device address
		const VkAccelerationStructureDeviceAddressInfoKHR info = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
			.accelerationStructure = vk_acceleration_structure,
		};
		obj._vkDeviceAddress = vkGetAccelerationStructureDeviceAddressKHR(_vkDevice, &info);

		return obj;
	}

	AccelerationStructure CTX::createAccelerationStructure(const AccelerationStructureSpec& spec) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);

		ASSERT_MSG(isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME), "VK_KHR_acceleration_structure is not enabled!");
		ASSERT_MSG(spec.indexType == VK_INDEX_TYPE_UINT16 || spec.indexType == VK_INDEX_TYPE_UINT32 || spec.indexType == VK_INDEX_TYPE_NONE_KHR, "Invalid indexType attribute, must be VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32, or VK_INDEX_TYPE_NONE_KHR.");

		AllocatedAccelerationStructure obj{};
		// _debugName
		snprintf(obj._debugName, sizeof(obj._debugName), "%s", spec.debugName);

		switch (spec.type) {
			case AccelerationStructureSpec::Type::BLAS: {
				obj = createBLASImpl(spec);
			} break;
			case AccelerationStructureSpec::Type::TLAS: {
				obj = createTLASImpl(spec);
			} break;
			default:
				ASSERT_MSG(false, "Invalid acceleration structure type choosen!");
		}

		vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(obj._vkAccelerationStructure), obj.getDebugName().data());
		const AccelerationStructureHandle handle = _accelerationStructurePool.create(std::move(obj));
		writeAccelerationStructureDescriptorsImpl(handle.index(), 1);
		return { this, handle };
	}

	// functions that wrap around existing functions for AllocatedObjects
	void CTX::generateMipmaps(TextureHandle handle) {
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot generate mipmaps immediately while a CommandBuffer is open, use IntermediateBuilder::generateMipmaps instead.");
		if (handle.empty()) {
			LOG_SYSTEM(LogType::Warning, "Generate mipmap with empty handle!");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		if (image->_numLevels <= 1) {
			LOG_SYSTEM(LogType::Warning, "Generate mipmap request of texture with less then 2 levels, returning early.");
			return;
		}
		const ImmediateCommands::CommandBufferWrapper& wrapper = _immGraphics->acquire();
		image->generateMipmap(wrapper._cmdBuf);
		_immGraphics->submit(wrapper);
	}
	void CTX::upload(BufferHandle handle, const void* data, size_t size, size_t offset) {
		MYTH_PROFILER_FUNCTION();
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		ASSERT_MSG(size > 0, "Size must be greater than 0!");
		ASSERT_MSG(handle.valid(), "CTX::upload called with an invalid BufferHandle.");

		AllocatedBuffer* buffer = _bufferPool.get(handle);
		ASSERT_MSG(buffer, "CTX::upload: handle does not resolve to a live buffer.");
		ASSERT_MSG(offset + size <= buffer->_bufferSize, "CTX::upload: offset + size ({}) exceeds buffer '{}' size ({}).", offset + size, buffer->_debugName, buffer->_bufferSize);
		ASSERT_MSG(buffer->isMapped() || (buffer->_vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), "CTX::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buffer->_debugName);
		_staging->bufferSubData(*buffer, offset, size, data);
	}
	void CTX::download(BufferHandle handle, void* data, size_t size, size_t offset) {
		MYTH_PROFILER_FUNCTION();
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Data is null");
			return;
		}
		AllocatedBuffer* buffer = _bufferPool.get(handle);

		if (!buffer) {
			LOG_SYSTEM(LogType::Error, "Retrieved buffer is null, handle must have been invalid!");
			return;
		}
		if (offset + size > buffer->_bufferSize) {
			LOG_SYSTEM(LogType::Error, "Buffer request to download is out of range!");
			return;
		}
		buffer->getBufferSubData(*this, offset, size, data);
	}
	void CTX::upload(TextureHandle handle, const void* data, const TexRange& range) {
		MYTH_PROFILER_FUNCTION();
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot upload a texture while a CommandBuffer is open, the immediate submit would run before it.");
		ASSERT_MSG(handle.valid(), "CTX::upload called with an invalid TextureHandle.");
		if (!data) {
			LOG_SYSTEM(LogType::Warning, "Attempting to upload data which is null!");
			return;
		}
		AllocatedTexture* image = _texturePool.get(handle);
		ASSERT_MSG(image, "Attempting to use texture via invalid handle!");
		ASSERT_MSG(image->_vkUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT, "CTX::upload: texture '{}' was not created with TRANSFER_DST usage.", image->_debugName);
		ASSERT_MSG(ValidateRange(image->_vkExtent, image->_numLevels, range), "CTX::upload: TexRange is out of bounds for texture '{}'.", image->_debugName);
		// why is this here
		if (image->_vkImageType == VK_IMAGE_TYPE_3D) {
			_staging->imageData3D(
			        *image, VkOffset3D{range.offset.x, range.offset.y, range.offset.z}, VkExtent3D{range.dimensions.width, range.dimensions.height, range.dimensions.depth}, image->_vkFormat, data
			);
		} else {
			const VkRect2D image_region = {
			    .offset = {.x = range.offset.x, .y = range.offset.y},
			    .extent = {.width = range.dimensions.width, .height = range.dimensions.height},
			};
			_staging->imageData2D(*image, image_region, range.mipLevel, range.numMipLevels, range.layer, range.numLayers, image->_vkFormat, data);
		}
	}
	void CTX::download(TextureHandle handle, void* data, const TexRange& range) {
		MYTH_PROFILER_FUNCTION();
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot download a texture while a CommandBuffer is open, the immediate submit would run before it.");
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
		_staging->getImageData(
		        *image,
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
		        data
		);
	}

	CommandBuffer& CTX::acquireCommand(CommandBuffer::Type type) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_ACQUIRE);
		ASSERT_MSG(!_currentCommandBuffer._ctx, "Cannot open more than 1 CommandBuffer on the same thread simultaneously!");
		_staging->resetPending();
		if (type == CommandBuffer::Type::Graphics) {
			VkSemaphore acquire_semaphore = _swapchain->acquire();
			_immGraphics->waitSemaphore(acquire_semaphore);
			wrappedBackBuffer.updateHandle(*this, _swapchain->getCurrentSwapchainTextureHandle());
		}
		_currentCommandBuffer = CommandBuffer(this, type);
		return _currentCommandBuffer;
	}
	SubmitHandle CTX::submitCommand(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_SUBMIT);
		ASSERT(cmd._ctx);
		ASSERT(cmd._wrapper);
#ifdef MYTH_ENABLED_TRACY_GPU
		TracyVkCollect(_tracyPlugin.getTracyVkCtx(), cmd._wrapper->_cmdBuf);
#endif
		const bool isPresenting = cmd._cmdType == CommandBuffer::Type::Graphics;
		if (isPresenting) {
			const uint32_t frameIndex = _swapchain->getCurrentFrameIndex();
			const uint64_t signalValue = _currentFrameNumber + _swapchain->getNumOfSwapchainImages();
			_swapchain->_timelineWaitValues[frameIndex] = signalValue;
			_immGraphics->signalSemaphore(_timelineSemaphore, signalValue);
		}
		cmd._lastSubmitHandle = _immGraphics->submit(*cmd._wrapper);
		_staging->onGraphSubmit(cmd._lastSubmitHandle);
		if (isPresenting) {
			const AllocatedTexture* backBuffer = _texturePool.get(_swapchain->getCurrentSwapchainTextureHandle());
			ASSERT(backBuffer->getImageLayout(backBuffer->getWholeRange()) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			_swapchain->present(_immGraphics->acquireLastSubmitSemaphore());
		}
		_currentFrameNumber++;
		processDeferredTasks();
		const SubmitHandle handle = cmd._lastSubmitHandle;
		_currentCommandBuffer = {};
		return handle;
	}
	void CTX::deferTask(std::packaged_task<void()>&& task, SubmitHandle handle) const {
		if (handle.empty()) {
			handle = _immGraphics->getNextSubmitHandle();
		}
		_deferredTasks.emplace_back(std::move(task), handle, _currentFrameNumber);
	}
	// void CTX::processDeferredTasks() {
	// 	auto it = _deferredTasks.begin();
	// 	while (it != _deferredTasks.end() && _imm->isReady(it->_handle, true)) {
	// 		(it++)->_task();
	// 	}
	// 	_deferredTasks.erase(_deferredTasks.begin(), it);
	// }
	//
	void CTX::processDeferredTasks() {
		if (isHeadless()) {
			auto it = _deferredTasks.begin();
			while (it != _deferredTasks.end()) {
				if (!_immGraphics->isReady(it->_handle, false))
					break;
				(it++)->_task();
			}
			_deferredTasks.erase(_deferredTasks.begin(), it);
			return;
		}
		const uint64_t numFramesToWait = _swapchain->getNumOfSwapchainImages();
		const uint64_t safeFrameThreshold = _currentFrameNumber > numFramesToWait ? _currentFrameNumber - numFramesToWait : 0;

		auto it = _deferredTasks.begin();
		while (it != _deferredTasks.end() && it->_frameNumber < safeFrameThreshold) {
			if (!_immGraphics->isReady(it->_handle, false)) {
				break;
			}
			(it++)->_task();
		}
		_deferredTasks.erase(_deferredTasks.begin(), it);
	}
	void CTX::waitDeferredTasks() {
		for (auto& task: _deferredTasks) {
			_immGraphics->wait(task._handle);
			task._task();
		}
		_deferredTasks.clear();
	}

	// 	SubmitHandle CTX::submitCommand(CommandBuffer& cmd) {
	// 		ASSERT(cmd._ctx);
	// 		ASSERT(cmd._wrapper);
	// #ifdef MYTH_ENABLED_TRACY_GPU
	// 		if (_tracyPlugin.isEnabled())
	// 			TracyVkCollect(_tracyPlugin.getTracyVkCtx(), cmd._wrapper->_cmdBuf);
	// #endif
	// 		const bool isPresenting = cmd._cmdType == CommandBuffer::Type::Graphics;
	// 		if (isPresenting) {
	// 			const uint64_t signalValue = _swapchain->_currentFrameNum + _swapchain->getNumOfSwapchainImages();
	// 			_swapchain->_timelineWaitValues[_swapchain->_currentImageIndex] = signalValue;
	// 			_imm->signalSemaphore(_timelineSemaphore, signalValue);
	// 		}
	//
	// 		cmd._lastSubmitHandle = _imm->submit(*cmd._wrapper);
	// 		if (isPresenting) {
	// 			ASSERT(_texturePool.get(_swapchain->getCurrentSwapchainTextureHandle())->getImageLayout() == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	// 			_swapchain->present();
	// 		}
	// 		processDeferredTasks();
	// 		SubmitHandle handle = cmd._lastSubmitHandle;
	// 		// reset
	// 		_currentCommandBuffer = {};
	// 		// might return the submit handle no clue
	// 		return handle;
	// 	}


	// come back here for reference
	//VkAccelerationStructureBuildSizesInfoKHR CTX::getBLASBuildInfo(const BottomLevelBuildDesc& desc) {
	//	if (!this->isExtensionEnabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
	//		LOG_SYSTEM(LogType::Warning, "VK_KHR_ACCELERATION_STRUCTURE is not an enabled extension, why are you calling 'getBLASBuildInfo()' then.");
	//
	//	VkAccelerationStructureGeometryDataKHR geometryData;
	//	switch (desc.geometryType) {
	//		case VK_GEOMETRY_TYPE_TRIANGLES_KHR: {
	//			VkAccelerationStructureGeometryTrianglesDataKHR geometry_triangles = {
	//				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
	//				.vertexFormat = desc.geometry.vertexFormat,
	//				.vertexData = { .deviceAddress = this->gpuAddress(desc.geometry.vertexBuffer) },
	//				.vertexStride = desc.geometry.vertexStride,
	//				.maxVertex = desc.geometry.numVertices,
	//				.indexType = desc.geometry.indexType,
	//				.indexData = { .deviceAddress = this->gpuAddress(desc.geometry.indexBuffer) }
	//			};
	//			geometryData = VkAccelerationStructureGeometryDataKHR(geometry_triangles);
	//		} break;
	//		case VK_GEOMETRY_TYPE_AABBS_KHR:
	//		case VK_GEOMETRY_TYPE_INSTANCES_KHR:
	//		default:
	//			ASSERT_MSG(false, "Geometry type not supported yet!");
	//	}
	//
	//	VkAccelerationStructureGeometryKHR geometry = {
	//		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
	//		.geometryType = desc.geometryType,
	//		.geometry = geometryData,
	//		.flags = desc.geometryFlags,
	//	};
	//	VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {
	//		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
	//		.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
	//		.flags = 0,
	//		.geometryCount = 1, // not sure yet
	//		.pGeometries = &geometry,
	//	};
	//
	//	VkAccelerationStructureBuildSizesInfoKHR outbuildSizes = {
	//		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
	//	};
	//	vkGetAccelerationStructureBuildSizesKHR(_vkDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildGeometryInfo, &buildGeometryInfo.geometryCount, &outbuildSizes);
	//	return outbuildSizes;
	//}

	void CTX::destroy(BufferHandle handle) {
		AllocatedBuffer* buf = _bufferPool.get(handle);
		if (!buf) {
			return;
		}
		deferTask(std::packaged_task<void()>([vma = _vmaAllocator, buffer = buf->_vkBuffer, allocation = buf->_vmaAllocation]() { vmaDestroyBuffer(vma, buffer, allocation); }));
		_bufferPool.destroy(handle);
	}
	void CTX::destroy(TextureHandle handle) {
		AllocatedTexture* image = _texturePool.get(handle);
		if (!image)
			return;
#ifdef MYTH_ENABLED_IMGUI
		if (_imguiPlugin.isEnabled()) {
			_imguiPlugin.removeImageDescriptor(handle);
		}
#endif
		deferTask(std::packaged_task<void()>([this, index = handle.index()]() {
			writeTextureDescriptorsImpl(index, 1);
			_texturePool.release(index);
		}));
		for (const auto& viewHandle : image->_additionalViews | std::views::values) {
			destroy(viewHandle);
		}
		image->_additionalViews.clear();
		deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageView]() { vkDestroyImageView(device, imageView, nullptr); }));
		if (image->_vkImageViewStorage) {
			deferTask(std::packaged_task<void()>([device = _vkDevice, imageView = image->_vkImageViewStorage]() { vkDestroyImageView(device, imageView, nullptr); }));
		}
		// necessary for swapchain imges which swapchain is created from
		if (image->_isOwning) {
			if (image->_mappedPtr) {
				vmaUnmapMemory(_vmaAllocator, image->_vmaAllocation);
			}
			deferTask(std::packaged_task<void()>([vma = _vmaAllocator, image = image->_vkImage, allocation = image->_vmaAllocation]() { vmaDestroyImage(vma, image, allocation); }));
		}
		_texturePool.retire(handle);
	}
	void CTX::destroy(SamplerHandle handle) {
		AllocatedSampler* sampler = _samplerPool.get(handle);
		if (!sampler)
			return;
#ifdef MYTH_ENABLED_IMGUI
		if (_imguiPlugin.isEnabled()) {
			_imguiPlugin.removeImageDescriptors(handle);
		}
#endif
		deferTask(std::packaged_task<void()>([this, index = handle.index()]() {
			writeSamplerDescriptorsImpl(index, 1);
			_samplerPool.release(index);
		}));
		deferTask(std::packaged_task<void()>([device = _vkDevice, sampler = sampler->_vkSampler]() { vkDestroySampler(device, sampler, nullptr); }));
		_samplerPool.retire(handle);
	}
	void CTX::destroy(ShaderHandle handle) {
		AllocatedShader* shader = _shaderPool.get(handle);
		if (!shader)
			return;
		deferTask(std::packaged_task<void()>([device = _vkDevice, module = shader->_vkShaderModule]() { vkDestroyShaderModule(device, module, nullptr); }));
		_shaderPool.destroy(handle);
	}

	void CTX::destroy(GraphicsPipelineHandle handle) {
		AllocatedGraphicsPipeline* graphics_pipeline = _graphicsPipelinePool.get(handle);
		if (!graphics_pipeline)
			return;
		PipelineCoreData& core = graphics_pipeline->_shared.core;
		// The bindless descriptor set layout is owned by CTX and shared across all pipelines.
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = core._vkPipeline]() { vkDestroyPipeline(device, pipeline, nullptr); }));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = core._vkPipelineLayout]() { vkDestroyPipelineLayout(device, layout, nullptr); }));
		_graphicsPipelinePool.destroy(handle);
	}
	void CTX::destroy(ComputePipelineHandle handle) {
		AllocatedComputePipeline* compute_pipeline = _computePipelinePool.get(handle);
		if (!compute_pipeline)
			return;
		PipelineCoreData& core = compute_pipeline->_shared.core;
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = core._vkPipeline]() { vkDestroyPipeline(device, pipeline, nullptr); }));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = core._vkPipelineLayout]() { vkDestroyPipelineLayout(device, layout, nullptr); }));
		_computePipelinePool.destroy(handle);
	}
	void CTX::destroy(RayTracingPipelineHandle handle) {
		AllocatedRayTracingPipeline* raytracing_pipeline = _rayTracingPipelinePool.get(handle);
		if (!raytracing_pipeline)
			return;
		PipelineCoreData& core = raytracing_pipeline->_shared.core;
		deferTask(std::packaged_task<void()>([device = _vkDevice, pipeline = core._vkPipeline]() { vkDestroyPipeline(device, pipeline, nullptr); }));
		deferTask(std::packaged_task<void()>([device = _vkDevice, layout = core._vkPipelineLayout]() { vkDestroyPipelineLayout(device, layout, nullptr); }));
		_rayTracingPipelinePool.destroy(handle);
	}
	void CTX::destroy(AccelerationStructureHandle handle) {
		AllocatedAccelerationStructure* acceleration_structure = _accelerationStructurePool.get(handle);
		if (!acceleration_structure)
			return;

		deferTask(std::packaged_task<void()>([this, index = handle.index()]() {
			writeAccelerationStructureDescriptorsImpl(index, 1);
			_accelerationStructurePool.release(index);
		}));
		deferTask(std::packaged_task<void()>([device = _vkDevice, accel_struct = acceleration_structure->_vkAccelerationStructure]() { vkDestroyAccelerationStructureKHR(device, accel_struct, nullptr); }));
		_accelerationStructurePool.retire(handle);
	}

	// ---- texture subresource views ----
	Dimensions CTX::getTextureDimensions(TextureHandle handle) const {
		return view(handle).getDimensions();
	}
	TextureViewKey CTX::createTextureView(TextureHandle handle, const TextureViewSpec& spec) {
		const TextureViewKey key = detail::packTextureViewKey(spec.mipLevel, spec.numMipLevels, spec.layer, spec.numLayers);
		AllocatedTexture& tex = access(handle);
		if (tex._additionalViews.find(key) != tex._additionalViews.end()) {
			return key; // already cached
		}
		VkImageViewType viewType = spec.type != VK_IMAGE_VIEW_TYPE_MAX_ENUM ? spec.type : tex.getViewType();
		if (spec.numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
			viewType = VK_IMAGE_VIEW_TYPE_2D;
		}
		char data[kMaxDebugNameLength];
		snprintf(data, sizeof(data), "%s - View (Mip: %d, NumMips: %d, Layer: %d, NumLayers: %d)", tex.getDebugName().data(), spec.mipLevel, spec.numMipLevels, spec.layer, spec.numLayers);
		TextureViewSpec resolvedSpec = spec;
		resolvedSpec.type = viewType;
		resolvedSpec.debugName = data;
		const TextureHandle newView = createTextureViewImpl(handle, resolvedSpec);
		access(handle)._additionalViews.emplace(key, newView);
		return key;
	}
	VkImageView CTX::getTextureView(TextureHandle handle, TextureViewKey key) const {
		const AllocatedTexture& tex = view(handle);
		auto it = tex._additionalViews.find(key);
		ASSERT_MSG(it != tex._additionalViews.end(), "TextureViewKey not found in texture!");
		return view(it->second).getImageView();
	}
	TextureHandle CTX::getTextureViewHandle(TextureHandle handle, TextureViewKey key) const {
		const AllocatedTexture& tex = view(handle);
		auto it = tex._additionalViews.find(key);
		ASSERT_MSG(it != tex._additionalViews.end(), "TextureViewKey not found in texture!");
		return it->second;
	}
} // namespace mythril
#ifdef MYTH_ENABLED_IMGUI
namespace mythril {
	namespace {
		class ScopedImGuiContext {
		public:
			explicit ScopedImGuiContext(ImGuiContext* next) :
			    _previous(ImGui::GetCurrentContext()) {
				ASSERT_MSG(next, "Cannot switch to a null ImGui context.");
				if (_previous != next) {
					ImGui::SetCurrentContext(next);
				}
			}

			~ScopedImGuiContext() {
				if (ImGui::GetCurrentContext() != _previous) {
					ImGui::SetCurrentContext(_previous);
				}
			}

		private:
			ImGuiContext* _previous = nullptr;
		};
	} // namespace

	VkDescriptorSet ImGuiPlugin::getOrCreateImageDescriptor(TextureHandle texHandle, SamplerHandle samplerHandle) {
		ImageDescriptorKey key{
		    .texture = texHandle,
		    .sampler = samplerHandle,
		};
		if (const auto iterator = _imageDescriptors.find(key); iterator != _imageDescriptors.end()) {
			return iterator->second;
		}

		ASSERT_MSG(_ctx, "Mythril ImGui plugin is missing its CTX.");
		const AllocatedTexture& texture = _ctx->view(texHandle);
		ASSERT_MSG(texture.isSampledImage(), "Texture '{}' must have sampled flag!", texture.getDebugName());

		const VkSampler sampler = samplerHandle.valid() ? _ctx->view(samplerHandle).getSampler() : _defaultSampler;
		ScopedImGuiContext context(_backendContext);
		VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(sampler, texture.getImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		_imageDescriptors.insert({key, ds});
		return ds;
	}

	void ImGuiPlugin::removeImageDescriptor(TextureHandle texHandle) {
		std::vector<VkDescriptorSet> descriptors;
		for (auto it = _imageDescriptors.begin(); it != _imageDescriptors.end();) {
			if (it->first.texture == texHandle) {
				descriptors.push_back(it->second);
				it = _imageDescriptors.erase(it);
			} else {
				++it;
			}
		}
		deferImageDescriptorRemoval(std::move(descriptors));
	}

	void ImGuiPlugin::removeImageDescriptors(SamplerHandle samplerHandle) {
		std::vector<VkDescriptorSet> descriptors;
		for (auto it = _imageDescriptors.begin(); it != _imageDescriptors.end();) {
			if (it->first.sampler == samplerHandle) {
				descriptors.push_back(it->second);
				it = _imageDescriptors.erase(it);
			} else {
				++it;
			}
		}
		deferImageDescriptorRemoval(std::move(descriptors));
	}

	void ImGuiPlugin::deferImageDescriptorRemoval(std::vector<VkDescriptorSet> descriptors) {
		if (descriptors.empty()) {
			return;
		}
		ASSERT_MSG(_ctx, "Mythril ImGui plugin is missing its CTX.");
		_ctx->deferTask(std::packaged_task<void()>([this, descriptors = std::move(descriptors)]() {
			ScopedImGuiContext context(_backendContext);
			for (VkDescriptorSet descriptor: descriptors) {
				ImGui_ImplVulkan_RemoveTexture(descriptor);
			}
		}));
	}

	void ImGuiPlugin::clearImageDescriptors() {
		ScopedImGuiContext context(_backendContext);
		for (const auto& [key, descriptor]: _imageDescriptors) {
			ImGui_ImplVulkan_RemoveTexture(descriptor);
		}
		_imageDescriptors.clear();
	}

	void ImGuiPlugin::renderImage(TextureHandle texHandle, SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		ASSERT_MSG(texHandle.valid(), "Texture handle is invalid!");
		ImGuiPlugin* plugin = _activePlugin;
		ASSERT_MSG(plugin && plugin->_ctx && plugin->_isEnabeld, "No active Mythril ImGui plugin is available for ImGui::Image(TextureHandle).");

		ImVec2 size;
		if (image_size.x <= 0 && image_size.y <= 0) {
			const Dimensions dims = plugin->_ctx->view(texHandle).getDimensions();
			size = {static_cast<float>(dims.width), static_cast<float>(dims.height)};
		} else {
			size = image_size;
		}

		VkDescriptorSet im_image_ds = plugin->getOrCreateImageDescriptor(texHandle, samplerHandle);
		ImGui::Image(reinterpret_cast<ImTextureID>(im_image_ds), size, uv0, uv1);
	}
} // namespace mythril
namespace ImGui {
	void Image(mythril::TextureHandle texHandle, mythril::SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		mythril::ImGuiPlugin::renderImage(texHandle, samplerHandle, image_size, uv0, uv1);
	}
	void Image(mythril::TextureHandle texHandle, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texHandle, mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, mythril::TextureViewKey viewKey, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		const mythril::TextureHandle viewHandle = texture.getContext()->getTextureViewHandle(texture.handle(), viewKey);
		Image(viewHandle, mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texture.handle(), mythril::SamplerHandle{}, image_size, uv0, uv1);
	}
	void Image(const mythril::Texture& texture, const mythril::Sampler& sampler, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1) {
		Image(texture.handle(), sampler.handle(), image_size, uv0, uv1);
	}

} // namespace ImGui
#endif
