//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "ObjectHandles.h"
#include "Shader.h"
#include "AccelerationStructure.h"
#include "PipelineSpecs.h"
#include "Objects.h"
#include "CommandBuffer.h"
#include "Plugins.h"
#include "Specs.h"
#include "SwapchainSpec.h"

#include <array>
#include <deque>
#include <filesystem>
#include <future>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <memory>


VK_DEFINE_HANDLE(VmaAllocator)
namespace mythril {
	// big forward declares!!!!
	class CTXBuilder;
	class CommandBuffer;
	class ImmediateCommands;
	class StagingDevice;
	class Swapchain;
	class SlangCompiler;

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
		CTX();
	public:
		~CTX();

		CTX(const CTX&) = delete;
		CTX& operator=(const CTX&) = delete;

		CTX(CTX&&) noexcept;
		CTX& operator=(CTX&&) noexcept;
	public:
		// arguements are optional as when not given we recreate with the same settings as the last swapchain
		void createSwapchain(const SwapchainSpec& spec = {});
		void destroySwapchain();
		// wraps create & destroy Swapchain functions as a most common use case
		void recreateSwapchain(const SwapchainSpec& spec = {});

		bool isSwapchainDirty() const;
		const TextureHandle& getCurrentSwapchainTexHandle() const;
		Dimensions getSwapchainDimensions() const;
		uint64_t getCurrentFrameNumber() const { return _currentFrameNumber; }
		VkDevice getVkDevice() const { return _vkDevice; }

		const Texture& getNullTexture() const { return this->_dummyTexture; }

		Buffer createBuffer(BufferSpec spec);
		void resizeBuffer(BufferHandle handle, size_t newSize);
		Texture createTexture(TextureSpec spec);
		void resizeTextureEXT(TextureHandle handle, Dimensions newDimensions);
		Dimensions getTextureDimensions(TextureHandle handle) const;
		bool isSwapchainTexture(TextureHandle handle) const;

		// subresource views; owned by the texture, freed on destroy
		TextureViewKey createTextureView(TextureHandle handle, const TextureViewSpec& spec);
		VkImageView getTextureView(TextureHandle handle, TextureViewKey key) const;
		TextureHandle getTextureViewHandle(TextureHandle handle, TextureViewKey key) const;
		Sampler createSampler(SamplerSpec spec);
		QueryPool createQueryPool(const QueryPoolSpec& spec);
		// host reset, caller guarantees the queries are not in flight (true for frame N - kNUM_FRAMES_IN_FLIGHT inside a render graph callback)
		void resetQueryPool(QueryPoolHandle handle, uint32_t first, uint32_t count) const;
		// non-blocking, returns false if any query in range is unavailable, values are ticks masked to timestampValidBits
		bool getQueryPoolResults(QueryPoolHandle handle, uint32_t first, std::span<uint64_t> outTicks) const;
		double timestampTicksToNs(uint64_t ticks) const;
		GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineSpec& spec);
		ComputePipeline createComputePipeline(const ComputePipelineSpec& spec);
		RayTracingPipeline createRayTracingPipeline(const RayTracingPipelineSpec& spec);
		Shader createShader(const ShaderSpec& spec);
		AccelerationStructure createAccelerationStructure(const AccelerationStructureSpec& spec);

		// for family
		VkDeviceAddress gpuAddress(const Buffer& buf, size_t offset = 0) const { return gpuAddress(buf.handle(), offset); }
		VkDeviceAddress gpuAddress(const AccelerationStructure& as) const { return gpuAddress(as.handle()); }
		// for friends
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
		void destroy(QueryPoolHandle handle);
		void destroy(ShaderHandle handle);
		void destroy(GraphicsPipelineHandle handle);
		void destroy(ComputePipelineHandle handle);
		void destroy(RayTracingPipelineHandle handle);
		void destroy(AccelerationStructureHandle handle);

		// helpers to query the capabilities of the Vulkan runtime
		bool isExtensionEnabled(std::string_view extensionName) const;
		// bytes in device local heaps, process wide driver numbers only when VK_EXT_memory_budget is enabled, otherwise vma allocations only
		size_t getGPUMemoryUsage() const;

		const VkPhysicalDeviceFeatures& getPhysicalDeviceFeatures10() const { return _featuresVulkan.features10; }
		const VkPhysicalDeviceVulkan11Features& getPhysicalDeviceFeatures11() const { return _featuresVulkan.features11; }
		const VkPhysicalDeviceVulkan12Features& getPhysicalDeviceFeatures12() const { return _featuresVulkan.features12; }
		const VkPhysicalDeviceVulkan13Features& getPhysicalDeviceFeatures13() const { return _featuresVulkan.features13; }

		const VkPhysicalDeviceProperties& getPhysicalDeviceProperties10() const { return _propertiesVulkan.props10; }
		const VkPhysicalDeviceVulkan11Properties& getPhysicalDeviceProperties11() const { return _propertiesVulkan.props11; }
		const VkPhysicalDeviceVulkan12Properties& getPhysicalDeviceProperties12() const { return _propertiesVulkan.props12; }
		const VkPhysicalDeviceVulkan13Properties& getPhysicalDeviceProperties13() const { return _propertiesVulkan.props13; }

		void waitIdle() const;

		// literally just for our node editor for now, experimental
		void switchShader(GraphicsPipeline& graphicsPipeline, Shader& newShader, ShaderStages stage);

		const detail::AllocatedShader& inspectShader(ShaderHandle handle) const;

		// for ray tracing ease of use, not clean as of now;
		// we want to reduce the user having to make calls like these since we do so behind the scenes in CTX later anyways
		void getTLASBuildInfo(const TopLevelBuildDesc& desc, VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo, VkAccelerationStructureGeometryKHR& outGeometry);
		void getBLASBuildInfo(const BottomLevelBuildDesc& desc, VkAccelerationStructureBuildSizesInfoKHR& outBuildSizesInfo, VkAccelerationStructureGeometryKHR& outGeometry);
	private:

		// internal detail:: accessors; user code goes through the handle-taking methods above
		// defined in lib/HandleAccess.h
		const detail::AllocatedTexture& view(TextureHandle h) const;
		const detail::AllocatedBuffer& view(BufferHandle h) const;
		const detail::AllocatedSampler& view(SamplerHandle h) const;
		const detail::AllocatedQueryPool& view(QueryPoolHandle h) const;
		const detail::AllocatedShader& view(ShaderHandle h) const;
		const detail::AllocatedGraphicsPipeline& view(GraphicsPipelineHandle h) const;
		const detail::AllocatedComputePipeline& view(ComputePipelineHandle h) const;
		const detail::AllocatedRayTracingPipeline& view(RayTracingPipelineHandle h) const;

		detail::AllocatedTexture& access(TextureHandle h);
		detail::AllocatedBuffer& access(BufferHandle h);
		detail::AllocatedSampler& access(SamplerHandle h);
		detail::AllocatedQueryPool& access(QueryPoolHandle h);
		detail::AllocatedShader& access(ShaderHandle h);
		detail::AllocatedGraphicsPipeline& access(GraphicsPipelineHandle h);
		detail::AllocatedComputePipeline& access(ComputePipelineHandle h);
		detail::AllocatedRayTracingPipeline& access(RayTracingPipelineHandle h);

		// wrappers around VulkanObjects
		// advanced functions that user will rarely need to call
		void generateMipmaps(TextureHandle handle);
		TextureHandle createTextureViewImpl(TextureHandle handle, TextureViewSpec spec);
		void createTextureViewObjectsImpl(detail::AllocatedTexture& view, const VkImageSubresourceRange& range);
		void resizeTextureImpl(TextureHandle handle, Dimensions newDimensions);
		void rebuildTextureViewsImpl(const detail::AllocatedTexture& image);

		// all things related to our pipeline constructions
		detail::PipelineCoreData buildPipelineCommonDataExceptVkPipelineImpl(const PipelineLayoutSignature& signature) const;

		// return values from resolvings are ignored for now
		void resolveGraphicsPipelineImpl(detail::AllocatedGraphicsPipeline& pipeline, uint32_t viewMask);
		void resolveComputePipelineImpl(detail::AllocatedComputePipeline& pipeline);
		void resolveRayTracingPipelineImpl(detail::AllocatedRayTracingPipeline& pipeline);
		void buildShaderBindingTableImpl(detail::AllocatedRayTracingPipeline& pipeline);
		void invalidateBindlessPipelineImpl(detail::SharedPipelineInfo& shared);

		VkPipeline buildComputePipelineImpl(VkPipelineLayout layout, ComputePipelineSpec spec);
		static void CheckSpecializationConstants(const detail::AllocatedShader& shader,
		                                         unsigned int expectedSpecConstantscount,
		                                         const char* shaderStageName,
		                                         const char* pipelineDebugName,
		                                         bool hasMultipleStages);
		VkPipeline buildRayTracingPipelineImpl(VkPipelineLayout layout, RayTracingPipelineSpec& spec);


		struct QueueFamilies {
			std::array<uint32_t, kQueueCount> indices{};
			uint32_t count = 0;
		};
		// distinct families of the queues in use, with more than one buffers and sampled-only images are concurrent
		QueueFamilies queueFamiliesInUse() const;
		// attachments and storage images stay exclusive across families so they keep compression, the graph transfers their ownership
		static bool NeedsOwnershipTransfer(VkImageUsageFlags usage) {
			return usage & (
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
				VK_IMAGE_USAGE_STORAGE_BIT);
		}

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
		        VkImageCreateFlags createFlags = 0,
		        bool requestConcurrent = false
		);

		detail::AllocatedAccelerationStructure createTLASImpl(const AccelerationStructureSpec& spec);
		detail::AllocatedAccelerationStructure createBLASImpl(const AccelerationStructureSpec& spec);


		void checkAndUpdateBindlessDescriptorSetImpl();
		void writeTextureDescriptorsImpl(uint32_t first, uint32_t count);
		void writeSamplerDescriptorsImpl(uint32_t first, uint32_t count);
		void growBindlessDescriptorPoolImpl(uint32_t newMaxSamplerCount, uint32_t newMaxTextureCount);

		// one frame, driven by RenderGraph::execute, nullptr when the swapchain is out of date
		[[nodiscard]] CommandBuffer* acquireCommand(bool presents);
		SubmitHandle submitCommand(CommandBuffer& cmd, bool presents);

		void waitForGraphUse(const detail::AllocatedBuffer& buffer) const;

		// pack tasks
		void deferTask(std::packaged_task<void()>&& task, SubmitHandle handle = SubmitHandle()) const;
		void processDeferredTasks();
		void waitDeferredTasks();

		bool isHeadless() const;
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
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR _rayTracingProperties {};
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

		mutable std::vector<DeferredTask> _deferredTasks;

		// for updating the bindless set
		bool _awaitingCreation = false;
		uint32_t _currentMaxTextureCount = 16;
		uint16_t _currentMaxSamplerCount = 16;

		VkDescriptorSetLayout _vkBindlessDSL = VK_NULL_HANDLE;
		VkDescriptorPool _vkBindlessDPool = VK_NULL_HANDLE;
		VkDescriptorSet _vkBindlessDSet = VK_NULL_HANDLE;

		VkSemaphore _timelineSemaphore = VK_NULL_HANDLE;

		CommandBuffer _currentCommandBuffer;
	private:
		// my stuff //
		uint64_t _currentFrameNumber = 0;
		std::unique_ptr<ImmediateCommands> _immGraphics;
		// both are nullptr when we fail to get a distinct family during setup
		std::unique_ptr<ImmediateCommands> _immAsyncCompute;
		std::unique_ptr<ImmediateCommands> _immAsyncTransfer;

		std::unique_ptr<Swapchain> _swapchain;
		std::unique_ptr<StagingDevice> _staging;

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
		HandlePool<QueryPoolHandle, detail::AllocatedQueryPool> _queryPoolPool;
		HandlePool<ShaderHandle, detail::AllocatedShader> _shaderPool;
		HandlePool<GraphicsPipelineHandle, detail::AllocatedGraphicsPipeline> _graphicsPipelinePool;
		HandlePool<ComputePipelineHandle, detail::AllocatedComputePipeline> _computePipelinePool;
		HandlePool<RayTracingPipelineHandle, detail::AllocatedRayTracingPipeline> _rayTracingPipelinePool;
		HandlePool<AccelerationStructureHandle, detail::AllocatedAccelerationStructure> _accelerationStructurePool;

		// some rare stuff
#ifdef MYTH_ENABLED_IMGUI
		ImGuiPlugin _imguiPlugin;
#endif
#ifdef MYTH_ENABLED_TRACY_GPU
		TracyGPUPlugin _tracyPlugin;
#endif
		std::unique_ptr<SlangCompiler> _slangCompiler;
		std::function<void(VkInstance, VkSurfaceKHR)> _surfaceDestroyFunc;

		friend class RenderGraph;
		friend class CommandBuffer;
		friend class TransferBuilder;
		friend class StagingDevice;
		friend class Swapchain;
		friend class CTXBuilder;
		friend class detail::AllocatedBuffer;
		friend class detail::AllocatedTexture;
		friend class ImGuiPlugin;
		friend class TracyGPUPlugin;

		template<typename T>
		friend class detail::Holder;
	};
} // namespace mythril
