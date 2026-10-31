//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "../../lib/HelperMacros.h"
#include "../../lib/ImmediateCommands.h"
#include "ObjectHandles.h"
#include "../../lib/StagingDevice.h"
#include "../../lib/Swapchain.h"

#include "../../lib/Pipelines.h"
#include "../../lib/Shader.h"
#include "../../lib/VulkanObjects.h"
#include "../../lib/AccelerationStructure.h"

#include "Objects.h"
#include "../../lib/CommandBuffer.h"
#include "../../lib/Plugins.h"
#include "../../lib/SlangCompiler.h"

#include "Specs.h"

#include <deque>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>

#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#endif

#if defined(MYTH_ENABLED_TRACY)
#include <tracy/Tracy.hpp>

// colors
// command buffer colors
#define MYTH_PROFILER_COLOR_WAIT 0x8a89a1 // blue gray
#define MYTH_PROFILER_COLOR_ACQUIRE 0x3128de // rich blue
#define MYTH_PROFILER_COLOR_SUBMIT 0x6b28de // dark purple

#define MYTH_PROFILER_COLOR_PRESENT 0xbf1bb7 // magenta
#define MYTH_PROFILER_COLOR_BARRIER 0xbdd1ff // baby blue
#define MYTH_PROFILER_COLOR_COMMAND 0x3f449e // light saturated blue

#define MYTH_PROFILER_COLOR_UPDATE 0xd1c12e // light yellow

// function colors
#define MYTH_PROFILER_COLOR_CREATE 0x22bf4f // dark green
#define MYTH_PROFILER_COLOR_DESTROY 0xc41212 // dark red


// rendering colors
#define MYTH_PROFILER_COLOR_RENDERGRAPH 0xf0784d // light orange
#define MYTH_PROFILER_COLOR_RENDERPASS 0xff591c // orange

// macro functions
#define MYTH_PROFILER_FUNCTION_N(name) ZoneScopedN(name)
#if defined(_MSC_VER)
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__FUNCSIG__)
#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__FUNCSIG__, color)
#else
#if defined(__APPLE__)
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__PRETTY_FUNCTION__)

#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__PRETTY_FUNCTION__, color)

#else
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__PRETTY_FUNCTION__)
#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__PRETTY_FUNCTION__, color)
#endif
#endif

#define MYTH_PROFILER_ZONE_COLOR(name, color)                                                                                                                                                          \
    {                                                                                                                                                                                                  \
        ZoneScopedNC(name, color);

#define MYTH_PROFILER_ZONE(name)                                                                                                                                                                       \
    {                                                                                                                                                                                                  \
        ZoneScopedN(name);

#define MYTH_PROFILER_ZONE_END() }


#define MYTH_PROFILER_THREAD(name) tracy::SetThreadName(name)
#define MYTH_PROFILER_FRAME() FrameMark;

#else
#define MYTH_PROFILER_FUNCTION()
#define MYTH_PROFILER_FUNCTION_N(name)
#define MYTH_PROFILER_FUNCTION_COLOR(color)
#define MYTH_PROFILER_ZONE_COLOR(name, color) {
#define MYTH_PROFILER_ZONE(name) {
#define MYTH_PROFILER_ZONE_END() }
#define MYTH_PROFILER_THREAD(name)
#define MYTH_PROFILER_FRAME(name)
#endif // MYTH_ENABLED_TRACY

#if defined(MYTH_ENABLED_TRACY_GPU)
#include <tracy/TracyVulkan.hpp>
#define MYTH_PROFILER_GPU_ZONE(ctx, name, cmdBuffer) TracyVkZoneTransient((ctx)->_tracyPlugin.getTracyVkCtx(), ___tracy_gpu_zone, cmdBuffer, name, true);
#else
#define MYTH_PROFILER_GPU_ZONE(ctx, name, cmdBuffer)
#endif // MYTH_ENABLED_TRACY_GPU

// metal only samples timestamps at encoder boundaries, per-command zones would split metal encoders
// aka you get horrible performance, so we disable it
#if defined(MYTH_ENABLED_TRACY_GPU) && !defined(__APPLE__)
#define MYTH_PROFILER_GPU_COMMAND_ZONE(name, cmdBuffer, color) TracyVkZoneC(this->_ctx->_tracyPlugin.getTracyVkCtx(), cmdBuffer, name, color);
#else
#define MYTH_PROFILER_GPU_COMMAND_ZONE(name, cmdBuffer, color)
#endif


namespace mythril {
	class CTXBuilder;
	class CommandBuffer;

	static constexpr uint32_t kMaxUserExtensions = 32;
	static constexpr VkFormat kDefaultSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
	static constexpr VkColorSpaceKHR kDefaultSwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	static constexpr VkPresentModeKHR kDefaultSwapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, SubmitHandle handle, uint64_t frameNum) :
		    _task(std::move(task)),
		    _handle(handle),
		    _frameNumber(frameNum) {}
		std::packaged_task<void()> _task;
		SubmitHandle _handle;
		uint64_t _frameNumber;
	};

	// helpers
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
		// aka a CTX not returned from CTXBuilder
		CTX() = default;
	public:
		~CTX();

		CTX(const CTX&) = delete;
		CTX& operator=(const CTX&) = delete;

		CTX(CTX&&) noexcept = default;
		CTX& operator=(CTX&&) noexcept = default;
	public:
		// arguements are optional as when not given we recreate with the same settings as the last swapchain
		void createSwapchain(const SwapchainSpec& spec = {});
		void destroySwapchain();
		// wraps create & destroy Swapchain functions as a most common use case
		void recreateSwapchain(const SwapchainSpec& spec = {});

		bool isSwapchainDirty() const {
			ASSERT_MSG(_swapchain, "Swapchain has not been created!");
			return _swapchain->isDirty();
		}
		const TextureHandle& getCurrentSwapchainTexHandle() const {
			ASSERT_MSG(_swapchain, "Swapchain has not been created!");
			return _swapchain->getCurrentSwapchainTextureHandle();
		}
		const Texture& getBackBufferTexture() {
			wrappedBackBuffer.updateHandle(*this, _swapchain->getCurrentSwapchainTextureHandle());
			return wrappedBackBuffer;
		}
		Dimensions getSwapchainDimensions() const {
			if (!_swapchain)
				return {lastSwapchainSpec.width, lastSwapchainSpec.height, 1};
			const VkExtent2D extent = _swapchain->getSwapchainExtent();
			return {extent.width, extent.height, 1};
		}

		const Texture& getNullTexture() const { return this->_dummyTexture; }

		Buffer createBuffer(BufferSpec spec);
		void resizeBuffer(BufferHandle handle, size_t newSize);
		Texture createTexture(TextureSpec spec);
		void resizeTexture(TextureHandle handle, Dimensions newDimensions);
		Dimensions getTextureDimensions(TextureHandle handle) const;
		bool isSwapchainTexture(TextureHandle handle) const { return view(handle).isSwapchainImage(); }

		// subresource views; owned by the texture, freed on destroy
		TextureViewKey createTextureView(TextureHandle handle, const TextureViewSpec& spec);
		VkImageView getTextureView(TextureHandle handle, TextureViewKey key) const;
		TextureHandle getTextureViewHandle(TextureHandle handle, TextureViewKey key) const;
		Sampler createSampler(SamplerSpec spec);
		GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineSpec& spec);
		ComputePipeline createComputePipeline(const ComputePipelineSpec& spec);
		RaytracingPipeline createRaytracingPipeline(const RaytracingPipelineSpec& spec);
		Shader createShader(const ShaderSpec& spec);
		AccelerationStructure createAccelerationStructure(const AccelerationStructureSpec& spec);

		VkDeviceAddress gpuAddress(BufferHandle handle, size_t offset = 0) const;
		VkDeviceAddress gpuAddress(AccelerationStructureHandle handle) const;

		// for buffers
		void upload(BufferHandle handle, const void* data, size_t size, size_t offset = 0);
		void download(BufferHandle handle, void* data, size_t size, size_t offset);
		// for textures
		void upload(TextureHandle handle, const void* data, const TexRange& range);
		void download(TextureHandle handle, void* data, const TexRange& range);

	public:
		// for automatic cleanup of resources
		void destroy(BufferHandle handle);
		void destroy(TextureHandle handle);
		void destroy(SamplerHandle handle);
		void destroy(ShaderHandle handle);
		void destroy(GraphicsPipelineHandle handle);
		void destroy(ComputePipelineHandle handle);
		void destroy(RaytracingPipelineHandle handle);
		void destroy(AccelerationStructureHandle handle);

		// helpers to query the capabilities of the Vulkan runtime
		bool isExtensionEnabled(std::string_view extensionName) const;

		const VkPhysicalDeviceFeatures& getPhysicalDeviceFeatures10() const { return _featuresVulkan.features10; }
		const VkPhysicalDeviceVulkan11Features& getPhysicalDeviceFeatures11() const { return _featuresVulkan.features11; }
		const VkPhysicalDeviceVulkan12Features& getPhysicalDeviceFeatures12() const { return _featuresVulkan.features12; }
		const VkPhysicalDeviceVulkan13Features& getPhysicalDeviceFeatures13() const { return _featuresVulkan.features13; }

		const VkPhysicalDeviceProperties& getPhysicalDeviceProperties10() const { return _propertiesVulkan.props10; }
		const VkPhysicalDeviceVulkan11Properties& getPhysicalDeviceProperties11() const { return _propertiesVulkan.props11; }
		const VkPhysicalDeviceVulkan12Properties& getPhysicalDeviceProperties12() const { return _propertiesVulkan.props12; }
		const VkPhysicalDeviceVulkan13Properties& getPhysicalDeviceProperties13() const { return _propertiesVulkan.props13; }

		// temp for now, for samples
		[[nodiscard]] CommandBuffer& acquireCommand(CommandBuffer::Type type);
		SubmitHandle submitCommand(CommandBuffer& cmd);

		// literally just for our node editor for now, experimental
		void switchShader(GraphicsPipeline& graphicsPipeline, Shader& newShader, ShaderStages stage);

		const detail::AllocatedShader& inspectShader(ShaderHandle handle) const { return view(handle); }

		// for ray tracing ease of use, not clean as of now;
		// we want to reduce the user having to make calls like these since we do so behind the scenes in CTX later anyways
		void getTLASBuildInfo(const TopLevelBuildDesc& desc, VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo, VkAccelerationStructureGeometryKHR& outGeometry);
		void getBLASBuildInfo(const BottomLevelBuildDesc& desc, VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo, VkAccelerationStructureGeometryKHR& outGeometry);
	private:

		// internal detail:: accessors; user code goes through the handle-taking methods above
		const detail::AllocatedTexture& view(TextureHandle h) const { return viewImpl(_texturePool, h); }
		const detail::AllocatedBuffer& view(BufferHandle h) const { return viewImpl(_bufferPool, h); }
		const detail::AllocatedSampler& view(SamplerHandle h) const { return viewImpl(_samplerPool, h); }
		const detail::AllocatedShader& view(ShaderHandle h) const { return viewImpl(_shaderPool, h); }
		const detail::AllocatedGraphicsPipeline& view(GraphicsPipelineHandle h) const { return viewImpl(_graphicsPipelinePool, h); }
		const detail::AllocatedComputePipeline& view(ComputePipelineHandle h) const { return viewImpl(_computePipelinePool, h); }
		const detail::AllocatedRaytracingPipeline& view(RaytracingPipelineHandle h) const { return viewImpl(_raytracingPipelinePool, h); }

		detail::AllocatedTexture& access(TextureHandle h) { return accessImpl(_texturePool, h); }
		detail::AllocatedBuffer& access(BufferHandle h) { return accessImpl(_bufferPool, h); }
		detail::AllocatedSampler& access(SamplerHandle h) { return accessImpl(_samplerPool, h); }
		detail::AllocatedShader& access(ShaderHandle h) { return accessImpl(_shaderPool, h); }
		detail::AllocatedGraphicsPipeline& access(GraphicsPipelineHandle h) { return accessImpl(_graphicsPipelinePool, h); }
		detail::AllocatedComputePipeline& access(ComputePipelineHandle h) { return accessImpl(_computePipelinePool, h); }
		detail::AllocatedRaytracingPipeline& access(RaytracingPipelineHandle h) { return accessImpl(_raytracingPipelinePool, h); }

		// wrappers around VulkanObjects
		// advanced functions that user will rarely need to call
		void generateMipmaps(TextureHandle handle);
		TextureHandle createTextureViewImpl(TextureHandle handle, TextureViewSpec spec);

		// all things related to our pipeline constructions
		detail::PipelineCoreData buildPipelineCommonDataExceptVkPipelineImpl(const PipelineLayoutSignature& signature);

		// return values from resolvings are ignored for now
		void resolveGraphicsPipelineImpl(detail::AllocatedGraphicsPipeline& pipeline, uint32_t viewMask);
		void resolveComputePipelineImpl(detail::AllocatedComputePipeline& pipeline);
		void resolveRaytracingPipelineImpl(detail::AllocatedRaytracingPipeline& pipeline);
		void buildShaderBindingTableImpl(detail::AllocatedRaytracingPipeline& pipeline);
		void invalidateBindlessPipelineImpl(detail::SharedPipelineInfo& shared);

		VkPipeline buildComputePipelineImpl(VkPipelineLayout layout, ComputePipelineSpec spec);
		static void CheckSpecializationConstants(const detail::AllocatedShader& shader,
		                                         unsigned int expectedSpecConstantscount,
		                                         const char* shaderStageName,
		                                         const char* pipelineDebugName,
		                                         bool hasMultipleStages);
		VkPipeline buildRayTracingPipelineImpl(VkPipelineLayout layout, RaytracingPipelineSpec& spec);


		// because they are big functions :(
		detail::AllocatedBuffer createBufferImpl(VkDeviceSize bufferSize, VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memFlags);
		detail::AllocatedTexture createTextureImpl(
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
		        VkImageCreateFlags createFlags = 0
		);

		detail::AllocatedAccelerationStructure createTLASImpl(const AccelerationStructureSpec& spec);
		detail::AllocatedAccelerationStructure createBLASImpl(const AccelerationStructureSpec& spec);


		void checkAndUpdateBindlessDescriptorSetImpl();
		void growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount, uint32_t newMaxAccelStructureCount);

		template<typename T>
		constexpr detail::PipelineCoreData& getPipelineCommonData(T handle) {
			static_assert(std::is_same_v<T, GraphicsPipelineHandle> || std::is_same_v<T, ComputePipelineHandle>);
			if constexpr (std::is_same_v<T, GraphicsPipelineHandle>) {
				auto* obj = _graphicsPipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			} else if constexpr (std::is_same_v<T, ComputePipelineHandle>) {
				auto* obj = _computePipelinePool.get(handle);
				ASSERT(obj);
				return obj->_common;
			}
			assert(false);
		}

		// pack tasks
		void deferTask(std::packaged_task<void()>&& task, SubmitHandle handle = SubmitHandle()) const;
		void processDeferredTasks();
		void waitDeferredTasks();

		bool isHeadless() const { return _swapchain == nullptr; }

		ImmediateCommands* getQueue(QueueAffinity type) const {
			// first two can be nullptr
			switch (type) {
				case QueueAffinity::AsyncCompute:  return _immAsyncCompute.get();
				case QueueAffinity::AsyncTransfer: return _immAsyncTransfer.get();
				default: return _immGraphics.get();
			}
		}
		ImmediateCommands* getQueueOrFallback(QueueAffinity type) const {
			ImmediateCommands* q = getQueue(type);
			return q ? q : _immGraphics.get();
		}
		uint32_t getQueueFamilyIndex(QueueAffinity type) const {
			switch (type) {
				case QueueAffinity::AsyncCompute:
					return (_immAsyncCompute) ? _computeQueueFamilyIndex : _graphicsQueueFamilyIndex;
				case QueueAffinity::AsyncTransfer:
					return (_immAsyncTransfer) ? _transferQueueFamilyIndex : _graphicsQueueFamilyIndex;
				default:
					return _graphicsQueueFamilyIndex;
			}
		}
	private:
		// Vulkan Members //
		VkInstance _vkInstance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT _vkDebugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR _vkSurfaceKHR = VK_NULL_HANDLE;
		VkPhysicalDevice _vkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice _vkDevice = VK_NULL_HANDLE;
		// VMA
		VmaAllocator _vmaAllocator = VK_NULL_HANDLE;

		// vulkan properties, features, and extensions we query
		VulkanFeatures _featuresVulkan {};
		VulkanProperties _propertiesVulkan {};
		// special case for extensions
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR _raytracingProperties {};
		VkPhysicalDeviceAccelerationStructurePropertiesKHR _accelerationStructureProperties {};
		struct TransparentStringHash {
			using is_transparent = void;
			size_t operator()(const std::string_view s) const { return std::hash<std::string_view>{}(s); }
		};
		std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> _enabledExtensionNames; // includes both instance and device extensions

		// queues
		VkQueue _vkGraphicsQueue = VK_NULL_HANDLE;
		uint32_t _graphicsQueueFamilyIndex = -1;
		// optional queues
		VkQueue _vkComputeQueue = VK_NULL_HANDLE;
		uint32_t _computeQueueFamilyIndex = -1;
		VkQueue _vkTransferQueue = VK_NULL_HANDLE;
		uint32_t _transferQueueFamilyIndex = -1;
		VkQueue _vkPresentQueue = VK_NULL_HANDLE;
		uint32_t _presentQueueFamilyIndex = -1;
	private:
		// not really my stuff //
		Texture _dummyTexture;
		Sampler _dummyLinearSampler;
		// every bindless acceleration structure slot must hold a TLAS, this one is never built so never trace against it
		VkAccelerationStructureKHR _dummyTLAS = VK_NULL_HANDLE;
		Buffer _dummyTLASBuffer;

		mutable std::vector<DeferredTask> _deferredTasks;

		// for updating the bindless set
		bool _awaitingCreation = false;
		bool _awaitingNewImmutableSamplers = false;
		uint32_t _currentMaxTextureCount = 16;
		uint16_t _currentMaxSamplerCount = 16;
		uint32_t _currentMaxAccelerationCount = 16;

		VkDescriptorSetLayout _vkBindlessDSL = VK_NULL_HANDLE;
		VkDescriptorPool _vkBindlessDPool = VK_NULL_HANDLE;
		VkDescriptorSet _vkBindlessDSet = VK_NULL_HANDLE;

		VkSemaphore _timelineSemaphore = VK_NULL_HANDLE;

		CommandBuffer _currentCommandBuffer;
	private:
		// my stuff //
		uint64_t _currentFrameNumber = 0;
		// bumped whenever graph-relevant resource topology changes (texture resize, swapchain recreate/destroy)
		uint64_t _resourceEpoch = 0;
		std::unique_ptr<ImmediateCommands> _immGraphics = nullptr;
		// both are nullptr when we fail to get a distinct family during setup
		std::unique_ptr<ImmediateCommands> _immAsyncCompute = nullptr;
		std::unique_ptr<ImmediateCommands> _immAsyncTransfer = nullptr;

		std::unique_ptr<Swapchain> _swapchain = nullptr;
		std::unique_ptr<StagingDevice> _staging = nullptr;

		Texture wrappedBackBuffer;
		// defaults
		SwapchainSpec lastSwapchainSpec{
		    .width = 1280,
		    .height = 720,
		    .format = kDefaultSwapchainFormat,
		    .colorSpace = kDefaultSwapchainColorSpace,
		    .presentMode = kDefaultSwapchainPresentMode,
		};

		HandlePool<BufferHandle, detail::AllocatedBuffer> _bufferPool;
		HandlePool<TextureHandle, detail::AllocatedTexture> _texturePool;
		HandlePool<SamplerHandle, detail::AllocatedSampler> _samplerPool;
		HandlePool<ShaderHandle, detail::AllocatedShader> _shaderPool;
		HandlePool<GraphicsPipelineHandle, detail::AllocatedGraphicsPipeline> _graphicsPipelinePool;
		HandlePool<ComputePipelineHandle, detail::AllocatedComputePipeline> _computePipelinePool;
		HandlePool<RaytracingPipelineHandle, detail::AllocatedRaytracingPipeline> _raytracingPipelinePool;
		HandlePool<AccelerationStructureHandle, detail::AllocatedAccelerationStructure> _accelerationStructurePool;

		// some rare stuff
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPlugin _imguiPlugin;
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		TracyGPUPlugin _tracyPlugin;
#endif
		SlangCompiler _slangCompiler;
		std::function<void(VkInstance, VkSurfaceKHR)> _surfaceDestroyFunc;

		friend class RenderGraph;
		friend class CommandBuffer;
		friend class IntermediateBuilder;
		friend class StagingDevice;
		friend class Swapchain;
		friend class CTXBuilder;
		friend class detail::AllocatedBuffer;
		friend class detail::AllocatedTexture;
		// plugins are usually friends
		friend class ImGuiPlugin;
		friend class TracyGPUPlugin;

		// wrappers need destroy()/updateHandle()
		template<typename T>
		friend class detail::Holder;
	};
} // namespace mythril
#ifdef MYTH_ENABLED_IMGUI
namespace ImGui {
	void Image(const mythril::Texture& texture, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(const mythril::Texture& texture, const mythril::Sampler& sampler, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(mythril::TextureHandle texHandle, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(mythril::TextureHandle texHandle, mythril::SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(const mythril::Texture& texture, mythril::TextureViewKey viewKey, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
} // namespace ImGui
#endif
