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
#include "Pipelines.h"
#include "Window.h"
#include "SlangCompiler.h"
#include "Plugins.h"
#include "DescriptorWriter.h"
#include "DescriptorAllocatorGrowable.h"

#include <future>
#include <deque>
#include <filesystem>
#include <unordered_map>

#include <volk.h>
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#endif


namespace mythril {
	class CTXBuilder;

	template<typename T>
	struct is_internal_object_handle : std::false_type {};
	template<typename Tag>
	struct is_internal_object_handle<InternalObjectHandle<Tag>> : std::true_type {};
	template<typename T>
	concept typenameInternalHandle = is_internal_object_handle<T>::value;


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

	enum class TextureType : uint8_t {
		Type_2D,
		Type_3D,
		Type_Cube
	};
	enum Swizzle : uint8_t {
		Swizzle_Default = 0,
		Swizzle_0,
		Swizzle_1,
		Swizzle_R,
		Swizzle_G,
		Swizzle_B,
		Swizzle_A,
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
	struct ComponentMapping
	{
		Swizzle r = Swizzle_Default;
		Swizzle g = Swizzle_Default;
		Swizzle b = Swizzle_Default;
		Swizzle a = Swizzle_Default;
		bool identity() const {
			return r == Swizzle_Default && g == Swizzle_Default && b == Swizzle_Default && a == Swizzle_Default;
		}
		VkComponentMapping toVkComponentMapping() const {
			return {
				.r = static_cast<VkComponentSwizzle>(r),
				.g = static_cast<VkComponentSwizzle>(g),
				.b = static_cast<VkComponentSwizzle>(b),
				.a = static_cast<VkComponentSwizzle>(a)
			};
		}
	};
	static_assert(mythril::Swizzle::Swizzle_Default == (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY);
	static_assert(mythril::Swizzle::Swizzle_0 == (uint32_t)VK_COMPONENT_SWIZZLE_ZERO);
	static_assert(mythril::Swizzle::Swizzle_1 == (uint32_t)VK_COMPONENT_SWIZZLE_ONE);
	static_assert(mythril::Swizzle::Swizzle_R == (uint32_t)VK_COMPONENT_SWIZZLE_R);
	static_assert(mythril::Swizzle::Swizzle_G == (uint32_t)VK_COMPONENT_SWIZZLE_G);
	static_assert(mythril::Swizzle::Swizzle_B == (uint32_t)VK_COMPONENT_SWIZZLE_B);
	static_assert(mythril::Swizzle::Swizzle_A == (uint32_t)VK_COMPONENT_SWIZZLE_A);



	// Specs should be low level but still a thin wrapper around the info creation processes need
	// User arguements will be even more abstract as they will not need to implement it via code
	struct SamplerSpec {
		SamplerFilter magFilter = SamplerFilter::Linear;
		SamplerFilter minFilter = SamplerFilter::Linear;
		SamplerMipMap mipMap = SamplerMipMap::Disabled;
		SamplerWrap wrapU = SamplerWrap::Repeat;
		SamplerWrap wrapV = SamplerWrap::Repeat;
		SamplerWrap wrapW = SamplerWrap::Repeat;

		bool depthCompareEnabled = false;
		CompareOp depthCompareOp = CompareOp::LessEqual;

		uint8_t mipLodMin = 0;
		uint8_t mipLodMax = 15;

		bool anistrophic = false;
		uint8_t maxAnisotropic = 1;
		const char* debugName = "Unnamed Sampler";
	};
	struct BufferSpec {
		size_t size = 0;
		uint8_t usage = {};
		StorageType storage = StorageType::HostVisible;
		const void* initialData = nullptr;
		const char* debugName = "Unnamed Buffer";
	};
	struct TextureSpec {
		VkExtent2D dimension = {};

		uint32_t numMipLevels = 1;
		uint32_t numLayers = 1;
		SampleCount samples = SampleCount::X1;

		uint8_t usage = {};
		StorageType storage = StorageType::Device;

		TextureType type = TextureType::Type_2D;
		VkFormat format = VK_FORMAT_UNDEFINED;
		ComponentMapping components = {};

		const void* initialData = nullptr;
		uint32_t dataNumMipLevels = 1; // how many mip-levels we want to upload
		bool generateMipmaps = false; // works only if initialData is not null
		const char* debugName = "Unnamed Texture";
	};

	struct TextureViewSpec {
		uint32_t layer = 0;
		uint32_t numLayers = 1;
		uint32_t mipLevel = 0;
		uint32_t numMipLevels = 1;
		ComponentMapping components = {};
	};
	struct ShaderSpec {
		std::filesystem::path filePath;
		const char* debugName = "Unnamed Shader";
	};



	struct LayoutBuildResult {
		std::vector<VkDescriptorSetLayout> allLayouts; // includes specials ie bindless set
		std::vector<VkDescriptorSetLayout> allocatableLayouts; // excludes specials
	};

	class CTX final {
		void construct(SwapchainArgs args);
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
		inline const InternalTextureHandle& getNullTexture() { return this->_dummyTextureHandle; };

		CommandBuffer& openCommand(CommandBuffer::Type type);
		SubmitHandle submitCommand(CommandBuffer& cmd);

		DescriptorSetWriter openDescriptorUpdate(InternalGraphicsPipelineHandle handle);
		DescriptorSetWriter openDescriptorUpdate(InternalComputePipelineHandle handle);
		void submitDescriptorUpdate(DescriptorSetWriter& updater);

		InternalBufferHandle createBuffer(BufferSpec spec);
		InternalTextureHandle createTexture(TextureSpec spec);
		InternalTextureHandle createTextureView(InternalTextureHandle handle, TextureViewSpec spec);
		void resizeTexture(InternalTextureHandle handle, VkExtent2D newExtent);
		InternalSamplerHandle createSampler(SamplerSpec spec);
		InternalGraphicsPipelineHandle createGraphicsPipeline(GraphicsPipelineSpec spec);
		InternalComputePipelineHandle createComputePipeline(ComputePipelineSpec spec);
		InternalShaderHandle createShader(ShaderSpec spec);

		VkDeviceAddress gpuAddress(InternalBufferHandle handle, size_t offset = 0);

		const AllocatedTexture& viewTexture(InternalTextureHandle handle) const {
			auto* ptr = _texturePool.get(handle);
			ASSERT_MSG(ptr, "Invalid texture handle!");
			return *ptr;
		}

		const AllocatedBuffer& viewBuffer(InternalBufferHandle handle) const {
			auto* ptr = _bufferPool.get(handle);
			ASSERT_MSG(ptr, "Invalid buffer handle!");
			return *ptr;
		}
		const AllocatedSampler& viewSampler(InternalSamplerHandle handle) const {
			auto* ptr = _samplerPool.get(handle);
			ASSERT_MSG(ptr, "Invalid sampler handle!");
			return *ptr;
		}
		const AllocatedShader& viewShader(InternalShaderHandle handle) const {
			auto* ptr = _shaderPool.get(handle);
			ASSERT_MSG(ptr, "Invalid shader handle!");
			return *ptr;
		}

		// wrappers around VulkanObjects
		// advanced functions that user will rarely need to call
		void transitionLayout(InternalTextureHandle handle, VkImageLayout newLayout, VkImageSubresourceRange range);
		void generateMipmaps(InternalTextureHandle handle);
		// for buffers
		void upload(InternalBufferHandle handle, const void* data, size_t size, size_t offset = 0);
		void download(InternalBufferHandle handle, void* data, size_t size, size_t offset);
		// for textures
		void upload(InternalTextureHandle handle, const void* data, const TexRange& range);
		void download(InternalTextureHandle handle, void* data, const TexRange& range);
	private:
		// for automatic cleanup of resources
		void destroy(InternalBufferHandle handle);
		void destroy(InternalTextureHandle handle);
		void destroy(InternalSamplerHandle handle);
		void destroy(InternalShaderHandle handle);
		void destroy(InternalGraphicsPipelineHandle handle);
		void destroy(InternalComputePipelineHandle handle);

		DescriptorSetWriter openUpdateImpl(PipelineCommon* common, const char* debugName);

		// all things related to our pipeline constructions
		PipelineCommon buildPipelineCommonDataExceptVkPipelineImpl(const PipelineLayoutSignature& signature);

		// return values from resolvings are ignored for now
		void resolveGraphicsPipelineImpl(AllocatedGraphicsPipeline& pipeline);
		void resolveComputePipelineImpl(AllocatedComputePipeline& pipeline);

		VkPipeline buildGraphicsPipelineImpl(VkPipelineLayout layout, GraphicsPipelineSpec spec);
		VkPipeline buildComputePipelineImpl(VkPipelineLayout layout, ComputePipelineSpec spec);


		// because they are big functions :(
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
										   VkComponentMapping componentMapping,
										   VkImageCreateFlags createFlags = 0);

		void checkAndUpdateBindlessDescriptorSetImpl();
		void growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount);


		template<typename T>
		constexpr PipelineCommon& getPipelienCommonData(T handle) {
			static_assert(
					std::is_same_v<T, InternalGraphicsPipelineHandle> ||
					std::is_same_v<T, InternalComputePipelineHandle>);
			if constexpr (std::is_same_v<T, InternalGraphicsPipelineHandle>) {
				auto* obj =_graphicsPipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			} else if constexpr (std::is_same_v<T, InternalComputePipelineHandle>) {
				auto* obj = _computePipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			}
		}

		LayoutBuildResult buildDescriptorResultFromSignature(const PipelineLayoutSignature &pipelineSignature);
		std::vector<VkDescriptorSet> allocateDescriptorSets(const std::vector<VkDescriptorSetLayout>& layouts);
		// pack tasks
		void deferTask(std::packaged_task<void()>&& task, SubmitHandle handle = SubmitHandle()) const;
		void processDeferredTasks();
		void waitDeferredTasks();

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
		VkPhysicalDeviceVulkan13Properties _vkPhysDeviceVulkan13Properties = {};
		VkPhysicalDeviceVulkan12Properties _vkPhysDeviceVulkan12Properties = {};
		VkPhysicalDeviceVulkan11Properties _vkPhysDeviceVulkan11Properties = {};

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
		DescriptorAllocatorGrowable _descriptorAllocator;
	private: // my stuff //
		std::unique_ptr<ImmediateCommands> _imm = nullptr;
		std::unique_ptr<Swapchain> _swapchain = nullptr;
		std::unique_ptr<StagingDevice> _staging = nullptr;

		HandlePool<InternalBufferHandle, AllocatedBuffer> _bufferPool;
		HandlePool<InternalTextureHandle, AllocatedTexture> _texturePool;
		HandlePool<InternalSamplerHandle, AllocatedSampler> _samplerPool;
		HandlePool<InternalShaderHandle, AllocatedShader> _shaderPool;
		HandlePool<InternalGraphicsPipelineHandle, AllocatedGraphicsPipeline> _graphicsPipelinePool;
		HandlePool<InternalComputePipelineHandle, AllocatedComputePipeline> _computePipelinePool;

		// some rare stuff
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPlugin _imguiPlugin;
#endif
		SlangCompiler _slangCompiler;
		Window _window;

		friend class DescriptorSetWriter;
		friend class RenderGraph;
		friend class CommandBuffer;
		friend class StagingDevice;
		friend class Swapchain;
		friend class CTXBuilder;
		friend struct AllocatedBuffer;
		friend struct AllocatedTexture;
		friend class ImGuiPlugin;

		// basically a lvk Holder
		template<typenameInternalHandle T>
		friend class ObjectHolder;
	};


#ifdef MYTH_ENABLED_IMGUI
	// data that is stored alongside th ImGui singleton so that it can be called from anywhere
	struct MyUserData {
		CTX* ctx;
		VkSampler sampler;
		std::unordered_map<InternalTextureHandle, VkDescriptorSet> handleMap;
	};
#endif
}
#ifdef MYTH_ENABLED_IMGUI
namespace ImGui {
	void Image(mythril::InternalTextureHandle texHandle, uint32_t mipLevel, const ImVec2 &image_size = {0, 0}, const ImVec2 &uv0 = {0, 0}, const ImVec2 &uv1 = {1, 1});
	void Image(mythril::InternalTextureHandle texHandle, const ImVec2 &image_size = {0, 0}, const ImVec2 &uv0 = {0, 0}, const ImVec2 &uv1 = {1, 1});
}
#endif


