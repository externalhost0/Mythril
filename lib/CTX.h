//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "HelperMacros.h"
#include "Swapchain.h"
#include "ImmediateCommands.h"
#include "StagingDevice.h"
#include "ObjectHandles.h"
#include "Window.h"

#include "VulkanObjects.h"
#include "Pipelines.h"
#include "Shader.h"

#include "Plugins.h"
#include "SlangCompiler.h"
#include "DescriptorWriter.h"
#include "DescriptorAllocatorGrowable.h"
#include "CommandBuffer.h"
#include "Specs.h"
#include "../include/mythril/Objects.h"

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

#if defined(MYTH_ENABLED_TRACY)
#include <tracy/Tracy.hpp>

// colors
// command buffer colors
#define MYTH_PROFILER_COLOR_WAIT  0x8a89a1 // blue gray
#define MYTH_PROFILER_COLOR_ACQUIRE 0x3128de // rich blue
#define MYTH_PROFILER_COLOR_SUBMIT 0x6b28de // dark purple
#define MYTH_PROFILER_COLOR_PRESENT 0xbf1bb7 // magenta
#define MYTH_PROFILER_COLOR_BARRIER 0xbdd1ff // baby blue
#define MYTH_PROFILER_COLOR_COMMAND 0x1a1b2e // light saturated blue

// function colors
#define MYTH_PROFILER_COLOR_CREATE 0x22bf4f // dark green
#define MYTH_PROFILER_COLOR_DESTROY 0xc41212 // dark red


// rendering colors
#define MYTH_PROFILER_COLOR_RENDERGRAPH 0xf0784d // light orange
#define MYTH_PROFILER_COLOR_RENDERPASS 0xff591c // orange

// macro functions
#if defined(_MSC_VER)
    #define MYTH_PROFILER_FUNCTION() ZoneScopedN(__FUNCSIG__)
	#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__FUNCSIG__, color)
#else
    #define MYTH_PROFILER_FUNCTION() ZoneScopedN(__PRETTY_FUNCTION__)
	#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__PRETTY_FUNCTION__, color)
#endif

#define MYTH_PROFILER_ZONE(name, color) { \
	ZoneScopedC(color); \
	ZoneName(name, strlen(name))
#define MYTH_PROFILER_ZONE_END() }

#define MYTH_PROFILER_THREAD(name) tracy::SetThreadName(name)
#define MYTH_PROFILER_FRAME(name) FrameMarkNamed(name)

#else
 #define MYTH_PROFILER_FUNCTION()
 #define MYTH_PROFILER_FUNCTION_COLOR(color)
 #define MYTH_PROFILER_ZONE(name, color) {
 #define MYTH_PROFILER_ZONE_END() }
 #define MYTH_PROFILER_THREAD(name)
 #define MYTH_PROFILER_FRAME(name)
#endif // MYTH_ENABLED_TRACY

#if defined(MYTH_ENABLED_TRACY_GPU)
	#include <tracy/TracyVulkan.hpp>
	#define MYTH_PROFILER_GPU_ZONE(name, cmdBuffer, color) TracyVkZoneC(this->_ctx->_tracyPlugin.getTracyVkCtx(), cmdBuffer, name, color);
#else
	#define MYTH_PROFILER_GPU_ZONE(name, cmdBuffer, color)
#endif // MYTH_ENABLED_TRACY_GPU


namespace mythril {
	class CTXBuilder;
	class CommandBuffer;

	// class Sampler;
	// class Buffer;
	// class Texture;
	// class Shader;
	// class GraphicsPipeline;
	// class ComputePipeline;

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, SubmitHandle handle) : _task(std::move(task)), _handle(handle) {}
		std::packaged_task<void()> _task;
		SubmitHandle _handle;
	};

	struct LayoutBuildResult {
		std::vector<VkDescriptorSetLayout> allLayouts; // includes specials ie bindless set
		std::vector<VkDescriptorSetLayout> allocatableLayouts; // excludes specials
	};

	//helpers
	template<typename Pool, typename Handle>
	const auto& viewImpl(const Pool& pool, Handle handle) {
		auto* ptr = pool.get(handle);
		ASSERT_MSG(ptr, "Invalid handle!");
		return *ptr;
	}
	template<typename Pool, typename Handle>
	static auto& accessImpl(Pool& pool, Handle handle) {
		auto* ptr = pool.get(handle);
		ASSERT_MSG(ptr, "Invalid handle!");
		return *ptr;
	}


	struct VulkanFeatures {
		VkPhysicalDeviceFeatures features10;
		VkPhysicalDeviceVulkan11Features features11;
		VkPhysicalDeviceVulkan12Features features12;
		VkPhysicalDeviceVulkan13Features features13;
	};
	struct VulkanProperties {
		VkPhysicalDeviceProperties props10;
		VkPhysicalDeviceVulkan11Properties props11;
		VkPhysicalDeviceVulkan12Properties props12;
		VkPhysicalDeviceVulkan13Properties props13;
	};


	class CTX final {
		void construct();
		// hide the default constructor to avoid user from instantiating a useless CTX
		CTX() = default;
	public:
		~CTX();

		CTX(const CTX &) = delete;
		CTX &operator=(const CTX &) = delete;

		CTX(CTX &&) noexcept = default;
		CTX &operator=(CTX &&) noexcept = default;
	public:

		// arguements are optional as when not given we recreate with the same settings as the last swapchain
		void createSwapchain(const SwapchainSpec& spec = {});
		void destroySwapchain();
		// wraps create & destroy Swapchain functions as a most common use case
		void recreateSwapchainStandard();

		bool isSwapchainDirty() const {
			ASSERT_MSG(_swapchain, "Swapchain has not been created!");
			return _swapchain->isDirty();
		}
		const TextureHandle& getCurrentSwapchainTexHandle() const {
			ASSERT_MSG(_swapchain, "Swapchain has not been created!");
			return _swapchain->getCurrentSwapchainTextureHandle();
		}
		const Texture& getBackBufferTexture() {
			wrappedBackBuffer.updateHandle(this, _swapchain->getCurrentSwapchainTextureHandle());
			return wrappedBackBuffer;
		}

		Window& getWindow() { return _window; }
		const Texture& getNullTexture() const { return this->_dummyTexture; }

		CommandBuffer& openCommand(CommandBuffer::Type type);
		SubmitHandle submitCommand(CommandBuffer& cmd);

		DescriptorSetWriter openDescriptorUpdate(const GraphicsPipeline& pipeline) { return openDescriptorUpdate(pipeline.handle()); }
		DescriptorSetWriter openDescriptorUpdate(const ComputePipeline& pipeline) { return openDescriptorUpdate(pipeline.handle()); }

		void submitDescriptorUpdate(DescriptorSetWriter& updater);

		Buffer createBuffer(BufferSpec spec);
		Texture createTexture(TextureSpec spec);
		void resizeTexture(TextureHandle handle, Dimensions newDimensions);
		Sampler createSampler(SamplerSpec spec);
		GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineSpec &spec);
		ComputePipeline createComputePipeline(const ComputePipelineSpec &spec);
		Shader createShader(const ShaderSpec &spec);

		VkDeviceAddress gpuAddress(BufferHandle handle, size_t offset = 0);

		// for buffers
		void upload(BufferHandle handle, const void* data, size_t size, size_t offset = 0);
		void download(BufferHandle handle, void* data, size_t size, size_t offset);
		// for textures
		void upload(TextureHandle handle, const void* data, const TexRange& range);
		void download(TextureHandle handle, void* data, const TexRange& range);
	public:

		const AllocatedTexture& view(TextureHandle h) const { return viewImpl(_texturePool, h); }
		const AllocatedBuffer& view(BufferHandle h) const { return viewImpl(_bufferPool, h); }
		const AllocatedSampler& view(SamplerHandle h) const { return viewImpl(_samplerPool, h); }
		const AllocatedShader& view(ShaderHandle h) const { return viewImpl(_shaderPool, h); }
		const AllocatedGraphicsPipeline& view(GraphicsPipelineHandle h) const { return viewImpl(_graphicsPipelinePool, h); }
		const AllocatedComputePipeline& view(ComputePipelineHandle h) const { return viewImpl(_computePipelinePool, h); }

		AllocatedTexture& access(TextureHandle h) { return accessImpl(_texturePool, h); }
		AllocatedBuffer& access(BufferHandle h) { return accessImpl(_bufferPool, h); }
		AllocatedSampler& access(SamplerHandle h) { return accessImpl(_samplerPool, h); }
		AllocatedShader& access(ShaderHandle h) { return accessImpl(_shaderPool, h); }
		AllocatedGraphicsPipeline& access(GraphicsPipelineHandle h) { return accessImpl(_graphicsPipelinePool, h); }
		AllocatedComputePipeline& access(ComputePipelineHandle h) { return accessImpl(_computePipelinePool, h); }

		// for automatic cleanup of resources
		void destroy(BufferHandle handle);
		void destroy(TextureHandle handle);
		void destroy(SamplerHandle handle);
		void destroy(ShaderHandle handle);
		void destroy(GraphicsPipelineHandle handle);
		void destroy(ComputePipelineHandle handle);

		// helpers to query the capabilities of the Vulkan runtime
		bool isExtensionEnabled(std::string_view extension_name) const;

		VkPhysicalDeviceFeatures getPhysicalDeviceFeatures10() const { return _featuresVulkan.features10; }
		VkPhysicalDeviceVulkan11Features getPhysicalDeviceFeatures11() const { return _featuresVulkan.features11; }
		VkPhysicalDeviceVulkan12Features getPhysicalDeviceFeatures12() const { return _featuresVulkan.features12; }
		VkPhysicalDeviceVulkan13Features getPhysicalDeviceFeatures13() const { return _featuresVulkan.features13; }

		VkPhysicalDeviceProperties getPhysicalDeviceProperties10() const { return _propertiesVulkan.props10; }
		VkPhysicalDeviceVulkan11Properties getPhysicalDeviceProperties11() const { return _propertiesVulkan.props11; }
		VkPhysicalDeviceVulkan12Properties getPhysicalDeviceProperties12() const { return _propertiesVulkan.props12; }
		VkPhysicalDeviceVulkan13Properties getPhysicalDeviceProperties13() const { return _propertiesVulkan.props13; }
	private:
		// wrappers around VulkanObjects
		// advanced functions that user will rarely need to call
		void transitionLayout(TextureHandle handle, VkImageLayout newLayout, VkImageSubresourceRange range);
		void generateMipmaps(TextureHandle handle);
		TextureHandle createTextureViewImpl(TextureHandle handle, TextureViewSpec spec);

		DescriptorSetWriter openDescriptorUpdate(GraphicsPipelineHandle handle);
		DescriptorSetWriter openDescriptorUpdate(ComputePipelineHandle handle);
		DescriptorSetWriter openUpdateImpl(PipelineCoreData* common, std::string_view debugName);

		// all things related to our pipeline constructions
		PipelineCoreData buildPipelineCommonDataExceptVkPipelineImpl(const PipelineLayoutSignature& signature);

		// return values from resolvings are ignored for now
		void resolveGraphicsPipelineImpl(AllocatedGraphicsPipeline& pipeline, uint32_t viewMask);
		void resolveComputePipelineImpl(AllocatedComputePipeline& pipeline);

		// VkPipeline buildGraphicsPipelineImpl(VkPipelineLayout layout, GraphicsPipelineSpec spec);
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
		constexpr PipelineCoreData& getPipelienCommonData(T handle) {
			static_assert(
					std::is_same_v<T, GraphicsPipelineHandle> ||
					std::is_same_v<T, ComputePipelineHandle>);
			if constexpr (std::is_same_v<T, GraphicsPipelineHandle>) {
				auto* obj =_graphicsPipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			} else if constexpr (std::is_same_v<T, ComputePipelineHandle>) {
				auto* obj = _computePipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			}
			assert(false);
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

		// vulkan properties, features, and extensions we query
		VulkanFeatures _featuresVulkan;
		VulkanProperties _propertiesVulkan;
		std::unordered_set<std::string> _enabledExtensionNames; // includes both instance and device extensions

		// vulkan queues
		VkQueue _vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t _graphicsQueueFamilyIndex = -1;
		// optional queues
		VkQueue _vkPresentQueue = VK_NULL_HANDLE;
		uint32_t _presentQueueFamilyIndex = -1;
		VkQueue _vkComputeQueue = VK_NULL_HANDLE;
		uint32_t _computeQueueFamilyIndex = -1;
	private: // not really my stuff //
		Texture _dummyTexture;
		Sampler _dummyLinearSampler;

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

		Texture wrappedBackBuffer;
		SwapchainSpec lastSwapchainSpec;

		HandlePool<BufferHandle, AllocatedBuffer> _bufferPool;
		HandlePool<TextureHandle, AllocatedTexture> _texturePool;
		HandlePool<SamplerHandle, AllocatedSampler> _samplerPool;
		HandlePool<ShaderHandle, AllocatedShader> _shaderPool;
		HandlePool<GraphicsPipelineHandle, AllocatedGraphicsPipeline> _graphicsPipelinePool;
		HandlePool<ComputePipelineHandle, AllocatedComputePipeline> _computePipelinePool;

		// some rare stuff
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPlugin _imguiPlugin;
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		TracyPlugin _tracyPlugin;
#endif
		SlangCompiler _slangCompiler;
		Window _window;

		friend class DescriptorSetWriter;
		friend class RenderGraph;
		friend class CommandBuffer;
		friend class StagingDevice;
		friend class Swapchain;
		friend class CTXBuilder;
		friend class AllocatedBuffer;
		friend class AllocatedTexture;
		friend class Texture;
		// plugins are usually friends
		friend class ImGuiPlugin;
		friend class TracyPlugin;

		// basically a lvk Holder
		template<typename T>
		friend class ObjectHolder;

	};



#ifdef MYTH_ENABLED_IMGUI
	// data that is stored alongside th ImGui singleton so that it can be called from anywhere
	struct MyUserData {
		CTX* ctx;
		VkSampler sampler;
		std::unordered_map<TextureHandle, VkDescriptorSet> handleMap;
	};
#endif
}
#ifdef MYTH_ENABLED_IMGUI
namespace ImGui {
	void Image(const mythril::Texture& texture, const ImVec2 &image_size = {0, 0}, const ImVec2 &uv0 = {0, 0}, const ImVec2 &uv1 = {1, 1});
	void Image(mythril::TextureHandle texHandle, const ImVec2 &image_size = {0, 0}, const ImVec2 &uv0 = {0, 0}, const ImVec2 &uv1 = {1, 1});
	void Image(const mythril::Texture& texture, const mythril::Texture::ViewKey& viewKey, const ImVec2& image_size = {0, 0}, const ImVec2& uv0={0, 0}, const ImVec2& uv1={1, 1});
}
#endif


