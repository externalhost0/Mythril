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
#include "Shader.h"
#include "GraphicsPipeline.h"
#include "Window.h"
#include "SlangCompiler.h"

#include <future>
#include <filesystem>
#include <volk.h>
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

namespace mythril {
	class CTXBuilder;

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
		const void* initialData = nullptr;
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

		const void* initialData = nullptr;
		uint32_t dataNumMipLevels = 1; // how many mip-levels we want to upload
		bool generateMipmaps = false;
		const char* debugName = "Unnamed Texture";
	};
	struct ShaderSpec {
		const char* filePath = nullptr;
		const char* debugName = "Unnamed Shader";
	};

	class DescriptorSet {
	public:
		explicit DescriptorSet(CTX& ctxRef) : _ctxRef(ctxRef) {}
		~DescriptorSet() = default;

		void writeBuffer();
		void updateSet();
	private:
		CTX& _ctxRef;

		std::vector<VkWriteDescriptorSet> _writes;
		std::queue<VkDescriptorBufferInfo> _bufferInfos;
	};


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

		CommandBuffer& openCommand(CommandBuffer::Type type);
		SubmitHandle submitCommand(CommandBuffer& cmd);

		void updateDescriptorSet(InternalBufferHandle handle, const char* paramName);

		InternalBufferHandle createBuffer(BufferSpec spec);
		InternalTextureHandle createTexture(TextureSpec spec);
		void resizeTexture(InternalTextureHandle handle, VkExtent2D newExtent);
		InternalSamplerHandle createSampler(SamplerSpec spec);
		InternalGraphicsPipelineHandle createGraphicsPipeline(GraphicsPipelineSpec spec);
		InternalShaderHandle createShader(ShaderSpec spec);

		VkDeviceAddress gpuAddress(InternalBufferHandle handle, size_t offset = 0);

		inline const AllocatedTexture& viewTexture(InternalTextureHandle handle) const {
			auto* ptr = _texturePool.get(handle);
			ASSERT_MSG(ptr, "Invalid texture handle!");
			return *ptr;
		}
		inline const AllocatedBuffer& viewBuffer(InternalBufferHandle handle) const {
			auto* ptr = _bufferPool.get(handle);
			ASSERT_MSG(ptr, "Invalid buffer handle!");
			return *ptr;
		}
		inline const AllocatedSampler& viewSampler(InternalSamplerHandle handle) const {
			auto* ptr = _samplerPool.get(handle);
			ASSERT_MSG(ptr, "Invalid sampler handle!");
			return *ptr;
		}
		inline const Shader& viewShader(InternalShaderHandle handle) const {
			auto* ptr = _shaderPool.get(handle);
			ASSERT_MSG(ptr, "Invalid shader handle!");
			return *ptr;
		}

		inline const AllocatedSampler& viewDefaultLinearSampler() const { return *_samplerPool.get(_dummyLinearSamplerHandle); }
	private:
		// for automatic cleanup of resources
		void destroy(InternalBufferHandle handle);
		void destroy(InternalTextureHandle handle);
		void destroy(InternalSamplerHandle handle);
		void destroy(InternalGraphicsPipelineHandle handle);
		void destroy(InternalShaderHandle handle);

		// helpers
		void generateMipmaps(InternalTextureHandle handle);
		GraphicsPipeline* resolveRenderPipeline(InternalGraphicsPipelineHandle handle);

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

		void bindDefaultBindlessDescriptorSetsImpl(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout);
		void checkAndUpdateBindlessDescriptorSetImpl();
		void growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount);

		// pack tasks
		void deferTask(std::packaged_task<void()>&& task, SubmitHandle handle = SubmitHandle()) const;
		void processDeferredTasks();
		void waitDeferredTasks();

		Slang::ComPtr<slang::ISession> createSlangSession();
	private: // Vulkan Members //
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
	private: // not really my stuff //
		InternalTextureHandle _dummyTextureHandle;
		InternalSamplerHandle _dummyLinearSamplerHandle;

		mutable std::vector<DeferredTask> _deferredTasks;

		bool _awaitingCreation = false;
		bool _awaitingNewImmutableSamplers = false;
		uint32_t _currentMaxTextureCount = 16;
		uint16_t _currentMaxSamplerCount = 16;

		VkDescriptorSetLayout _vkBindlessDSL = VK_NULL_HANDLE;
		VkDescriptorPool _vkBindlessDPool = VK_NULL_HANDLE;
		VkDescriptorSet _vkBindlessDSet = VK_NULL_HANDLE;

		VkSemaphore _timelineSemaphore = VK_NULL_HANDLE;
		CommandBuffer _currentCommandBuffer;
	private: // my stuff //
		std::unique_ptr<ImmediateCommands> _imm = nullptr;
		std::unique_ptr<Swapchain> _swapchain = nullptr;
		std::unique_ptr<StagingDevice> _staging = nullptr;

		HandlePool<InternalBufferHandle, AllocatedBuffer> _bufferPool;
		HandlePool<InternalTextureHandle, AllocatedTexture> _texturePool;
		HandlePool<InternalSamplerHandle, AllocatedSampler> _samplerPool;
		HandlePool<InternalShaderHandle, Shader> _shaderPool;
		HandlePool<InternalGraphicsPipelineHandle, GraphicsPipeline> _graphicsPipelinePool;
		HandlePool<InternalComputePipelineHandle, ComputePipeline> _computePipelinePool;

		// some rare stuff
		std::vector<std::unique_ptr<class BasePlugin>> _plugins = {};
		SlangCompiler _slangCompiler;
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
