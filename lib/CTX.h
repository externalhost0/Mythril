//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "HelperMacros.h"
#include "Swapchain.h"
#include "ImmediateCommands.h"
#include "StagingDevice.h"
#include "CommandBuffer.h"
#include "VulkanObjects.h"
#include "ObjectHandles.h"
#include "RenderPipeline.h"
#include "Window.h"

#include <future>
#include <filesystem>
#include <volk.h>
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

namespace mythril {
	class CTXBuilder;


	struct CompiledShaderData {
		VkShaderModule vkShaderModule = VK_NULL_HANDLE;
		size_t pushConstantSize = 0;
	};

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, SubmitHandle handle) : _task(std::move(task)), _handle(handle) {}
		std::packaged_task<void()> _task;
		SubmitHandle _handle;
	};

	// bits need to be uint8_t underlying type
	enum BufferUsageBits : uint8_t {
		BufferUsageBits_Index = 1 << 0,
		BufferUsageBits_Uniform = 1 << 1,
		BufferUsageBits_Storage = 1 << 2,
		BufferUsageBits_Indirect = 1 << 3
	};
	enum TextureUsageBits : uint8_t {
		TextureUsageBits_Sampled = 1 << 0,
		TextureUsageBits_Storage = 1 << 1,
		TextureUsageBits_Attachment = 1 << 2,
	};

	// strict enums can be whatever
	enum class StorageType : uint8_t {
		Device,
		HostVisible,
		Memoryless
	};

	enum class TextureType {
		Type_2D,
		Type_3D,
		Type_Cube
	};
	struct TexRange
	{
		VkOffset3D offset = {};
		VkExtent3D dimensions = {1, 1, 1};

		uint32_t layer = 0;
		uint32_t numLayers = 1;
		uint32_t mipLevel = 0;
		uint32_t numMipLevels = 1;
	};

	// Specs should be low level but still a thin wrapper around the info creation processes need
	// User arguements will be even more abstract as they will not need to implement it via code
	struct SamplerSpec {
		SamplerFilter magFilter = SamplerFilter::Linear;
		SamplerFilter minFilter = SamplerFilter::Linear;
		SamplerWrap wrapU = SamplerWrap::Repeat;
		SamplerWrap wrapV = SamplerWrap::Repeat;
		SamplerWrap wrapW = SamplerWrap::Repeat;

		SamplerMip mipMap = SamplerMip::Disabled;
		bool anistrophic = true;
		const char* debugName = "Unnamed Sampler";
	};
	struct BufferSpec {
		size_t size = 0;
		uint8_t usage = {};
		StorageType storage = StorageType::HostVisible;
		const void* data = nullptr;
		const char* debugName = "Unnamed Buffer";
	};
	enum class ResolutionMode {
		Logical,
		Physical
	};
	struct TextureSpec {
		VkExtent2D dimension = {};
		ResolutionMode resolutionMode = ResolutionMode::Logical;

		uint32_t numMipLevels = 1;
		uint32_t numLayers = 1;
		SampleCount samples = SampleCount::X1;

		uint8_t usage = {};
		StorageType storage = StorageType::Device;

		TextureType type = TextureType::Type_2D;
		VkFormat format = VK_FORMAT_UNDEFINED;

		const void* data = nullptr;
		uint32_t dataNumMipLevels = 1; // how many mip-levels we want to upload
		bool generateMipmaps = false;
		const char* debugName = "Unnamed Texture";
	};
	struct ShaderSpec {
		const char* filePath = nullptr;
		const char* debugName = "Unnamed Shader";
	};

	enum class WindowMode;

	class CTX final {
	public:
		CTX() = default;
		~CTX();
		CTX(const CTX &) = delete;
		CTX &operator=(const CTX &) = delete;
		CTX(CTX &&) noexcept = default;
		CTX &operator=(CTX &&) noexcept = default;
	public:
		void cleanSwapchain();
		bool isSwapchainDirty();
		Window& getWindow() { return _window; };

		void waitIdle() {
			//fixme: find more performant solution for this, im just lazy rn
			VK_CHECK(vkDeviceWaitIdle(_vkDevice));
		};

		CommandBuffer& openCommand(CommandBuffer::Type type);
		SubmitHandle submitCommand(CommandBuffer& cmd);

		InternalBufferHandle createBuffer(BufferSpec spec);
		InternalTextureHandle createTexture(TextureSpec spec);
		void resizeTexture(InternalTextureHandle handle, VkExtent2D newExtent);
		InternalSamplerHandle createSampler(SamplerSpec spec);
		InternalPipelineHandle createPipeline(PipelineSpec spec);
		InternalShaderHandle createShader(ShaderSpec spec);

		VkDeviceAddress gpuAddress(InternalBufferHandle handle, size_t offset = 0);

		const AllocatedTexture& getTexture(InternalTextureHandle handle) const { return *_texturePool.get(handle); };
		const AllocatedBuffer& getBuffer(InternalBufferHandle handle) const { return *_bufferPool.get(handle); }
		const AllocatedSampler& getSampler(InternalSamplerHandle handle) const { return *_samplerPool.get(handle); }

		const AllocatedSampler& getDefaultLinearSampler() const { return *_samplerPool.get(_linearSamplerHandle); }
		const AllocatedSampler& getDefaultNearestSampler() const { return *_samplerPool.get(_nearestSamplerHandle); }
	private:
		// for automatic cleanup of resources
		void destroy(InternalBufferHandle handle);
		void destroy(InternalTextureHandle handle);
		void destroy(InternalSamplerHandle handle);
		void destroy(InternalPipelineHandle handle);
		void destroy(InternalShaderHandle handle);

		// helpers
		void generateMipmaps(InternalTextureHandle handle);
		RenderPipeline* resolveRenderPipeline(InternalPipelineHandle handle);

		void upload(InternalBufferHandle handle, const void* data, size_t size, size_t offset = 0);
		void download(InternalBufferHandle handle, void* data, size_t size, size_t offset);

		void upload(InternalTextureHandle handle, const void* data, const TexRange& range);
		void download(InternalTextureHandle handle, void* data, const TexRange& range);

		AllocatedBuffer createBufferImpl(VkDeviceSize bufferSize, VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memFlags);
		AllocatedTexture createTextureImpl(VkImageUsageFlags usageFlags,
										   VkMemoryPropertyFlags memFlags,
										   VkExtent3D extent3D,
										   VkFormat format,
										   VkImageType imageType,
										   VkImageViewType imageViewType,
										   uint32_t numLevels,
										   uint32_t numLayers,
										   VkSampleCountFlagBits sampleCountFlagBits,
										   VkImageCreateFlags createFlags = 0);


		// confusing things
		void bindDefaultDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout);
		void checkAndUpdateDescriptorSets();
		void growDescriptorPool(uint32_t newMaxTextureCount, uint16_t newMaxSamplerCount);
		// pack tasks
		void deferTask(std::packaged_task<void()>&& task, SubmitHandle handle = SubmitHandle()) const;
		void processDeferredTasks();
		void waitDeferredTasks();
	private:
		// Vulkan
		VkInstance _vkInstance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT _vkDebugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR _vkSurfaceKHR = VK_NULL_HANDLE;
		VkPhysicalDevice _vkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice _vkDevice = VK_NULL_HANDLE;
		// VMA
		VmaAllocator _vmaAllocator = VK_NULL_HANDLE;

		// vulkan properties
		VkPhysicalDeviceProperties _vkPhysDeviceProperties = {};
#if defined(VK_API_VERSION_1_3)
		VkPhysicalDeviceVulkan13Properties _vkPhysDeviceVulkan13Properties = {};
#endif
#if defined(VK_API_VERSION_1_2)
		VkPhysicalDeviceVulkan12Properties _vkPhysDeviceVulkan12Properties = {};
#endif
#if defined(VK_API_VERSION_1_1)
		VkPhysicalDeviceVulkan11Properties _vkPhysDeviceVulkan11Properties = {};
#endif

		// vulkan queues
		VkQueue _vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t _graphicsQueueFamilyIndex = -1;
		VkQueue _vkPresentQueue = VK_NULL_HANDLE;
		uint32_t _presentQueueFamilyIndex = -1;
	private:
		InternalSamplerHandle _linearSamplerHandle;
		InternalSamplerHandle _nearestSamplerHandle;
		InternalTextureHandle _dummyTextureHandle;

		mutable std::vector<DeferredTask> _deferredTasks;

		bool _awaitingCreation = false;
		bool _awaitingNewImmutableSamplers = false;
		uint32_t _currentMaxTextureCount = 16;
		uint16_t _currentMaxSamplerCount = 16;

		VkDescriptorSetLayout _vkDSL = VK_NULL_HANDLE;
		VkDescriptorPool _vkDPool = VK_NULL_HANDLE;
		VkDescriptorSet _vkDSet = VK_NULL_HANDLE;

		VkSemaphore _timelineSemaphore = VK_NULL_HANDLE;
		CommandBuffer _currentCommandBuffer;
	private:
		// my stuff
		std::unique_ptr<ImmediateCommands> _imm = nullptr;
		std::unique_ptr<Swapchain> _swapchain = nullptr;
		std::unique_ptr<StagingDevice> _staging = nullptr;

		HandlePool<InternalBufferHandle, AllocatedBuffer> _bufferPool;
		HandlePool<InternalTextureHandle, AllocatedTexture> _texturePool;
		HandlePool<InternalSamplerHandle, AllocatedSampler> _samplerPool;
		HandlePool<InternalPipelineHandle, RenderPipeline> _pipelinePool;
		HandlePool<InternalShaderHandle, CompiledShaderData> _shaderPool;

		// some rare stuff
		std::vector<std::unique_ptr<class BasePlugin>> _plugins = {};
		Slang::ComPtr<slang::ISession> _slangSession = nullptr;
		Window _window;

		friend class RenderGraph;
		friend class CommandBuffer;
		friend class StagingDevice;
		friend class Swapchain;
		friend class CTXBuilder;
		friend class AllocatedBuffer;
		friend class AllocatedTexture;
		friend class ImGuiPlugin;
	};

}
