//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include "ObjectHandles.h"
#include "RenderGraphBuilder.h"
#include "SubmitHandle.h"
#include "vkenums.h"

#include <volk.h>

#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace mythril {
	// forward declarations
	class CTX;
	namespace detail {
		class AllocatedBuffer;
		class AllocatedTexture;
		struct CommandBufferWrapper;
		struct PipelineCoreData;
		struct SharedPipelineInfo;
	}

	// we only use it for the cmdBeginRendering command anyways
	struct Dependencies {
		enum {
			kMaxSubmitDependencies = 4
		};
		TextureHandle textures[kMaxSubmitDependencies] = {};
		BufferHandle buffers[kMaxSubmitDependencies] = {};
	};

	struct DepthState {
		CompareOp compareOp = CompareOp::AlwaysPass;
		bool isDepthWriteEnabled = false;
	};

	// lets RAII this guy
	class CommandBuffer final {
	public:
		CommandBuffer() = default;
		explicit CommandBuffer(CTX* ctx);
		~CommandBuffer();

	private:
		CommandBuffer(CTX* ctx, const detail::CommandBufferWrapper& wrapper, QueueAffinity queue) : _ctx(ctx), _wrapper(&wrapper), _queue(queue) {}
		ResolvedTexture texture(GraphTexture texture) const;
		VkBuffer vkBuffer(GraphBuffer buffer) const;
		VkImage vkImage(GraphTexture texture) const;

		void cmdBindComputePipeline(ComputePipelineHandle handle);
		void cmdBindGraphicsPipeline(GraphicsPipelineHandle handle);
		void cmdBindRayTracingPipeline(RayTracingPipelineHandle handle);
		void cmdBindDepthState(const DepthState& state);
		void cmdSetDepthBiasEnable(bool enable);
		void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp);

		void cmdBeginRenderingEXT(uint32_t layerCount = 1, uint32_t viewMask = 0b000000);
		void cmdEndRenderingEXT();

		void cmdDispatchThreadGroup(const Dimensions& threadGroupCount);
		void cmdDispatchIndirect(const Buffer& indirectBuffer, VkDeviceSize offset = 0);

		void cmdTraceRays(const Dimensions& queryDimension);

		void cmdBindIndexBuffer(BufferHandle buffer);
		void cmdPushConstants(const void* data, uint32_t size, uint32_t offset);
		void cmdUpdateBufferEXT(BufferHandle handle, size_t offset, size_t size, const void* data);

		void cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
		void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t baseInstance);
		void cmdDrawIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
		void cmdDrawIndexedIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
		void cmdDrawIndexedIndirectCount(const Buffer& indirectBuffer, VkDeviceSize offset, const Buffer& countBuffer, VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride);

		void cmdWriteTimestamp(QueryPoolHandle pool, uint32_t query);
		void cmdResetQueryPool(QueryPoolHandle pool, uint32_t first, uint32_t count);

#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui();
#endif
		// TODO: safegaurd these commands
		void cmdGenerateMipmap(TextureHandle handle);
		void cmdGenerateMipmap(const Texture& texture) { cmdGenerateMipmap(texture.handle()); }

		void cmdTransitionLayout(const Texture& source, VkImageLayout newLayout) { cmdTransitionLayout(source.handle(), newLayout); }
		void cmdCopyImage(const Texture& source, const Texture& destination) { cmdCopyImage(source.handle(), destination.handle()); }
		void cmdBlitImage(const Texture& source, const Texture& destination) { cmdBlitImage(source.handle(), destination.handle()); }
		void cmdCopyImageToBuffer(const Texture& source, const Buffer& destination, const VkBufferImageCopy& region) { cmdCopyImageToBuffer(source.handle(), destination.handle(), region); }

		void cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout, VkImageSubresourceRange range);
		void cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout);
		void cmdCopyImage(TextureHandle source, TextureHandle destination);
		void cmdBlitImage(TextureHandle source, TextureHandle destination);
		void cmdCopyImageToBuffer(TextureHandle source, BufferHandle destination, const VkBufferImageCopy& region);
	private:
		void cmdBlitImageToSwapchain(TextureHandle source);
		void cmdCopyImageToSwapchain(TextureHandle source);
		// just repeated logic
		void cmdBindPipelineImpl(const detail::PipelineCoreData* common, VkPipelineBindPoint bindPoint);

		// all functions that still have equivalent Vulkan commands but should be abstracted away from user
		void cmdBeginRenderingImpl(uint32_t layerCount, uint32_t viewMask);
		void cmdEndRenderingImpl();
		void resetMemberStateImpl();


		void cmdCopyImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D size);
		void cmdBlitImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize);

		void cmdSetViewportImpl(VkExtent2D extent2D);
		void cmdSetScissorImpl(VkExtent2D extent2D);

		void bufferBarrierImpl(const detail::AllocatedBuffer& buf, StageAccess src, StageAccess dst);
		void orderAfterPreviousDispatchImpl();

		// helpers
		void CheckTextureRenderingUsage(const detail::AllocatedTexture& source, const detail::AllocatedTexture& destination, const char* operation) const;
		void CheckImageLayoutAuto(TextureHandle sourceHandle, TextureHandle destinationHandle, const char* operation);

	private:
		// pretty important members for communication to the rest of the renderer
		CTX* _ctx = nullptr;
		const detail::CommandBufferWrapper* _wrapper = nullptr;
		QueueAffinity _queue = QueueAffinity::Graphics;

		// all set via RenderGraph
		VkPipeline _lastBoundvkPipeline = VK_NULL_HANDLE;

		// avoid lookup and store the common data
		detail::SharedPipelineInfo* _currentPipelineInfo = nullptr;
		// todo: kinda sloppy since you have to manually update
		std::variant<GraphicsPipelineHandle, ComputePipelineHandle, RayTracingPipelineHandle> _currentPipelineHandle;
		const CompiledPass* _activePass = nullptr;
		const RenderGraph* _graph = nullptr;
		const PassMember* _activeMember = nullptr;

		bool _isRendering = false; // cmdBeginRendering
		bool _hasDispatchedInPass = false;
#ifdef DEBUG
		// a pipeline whose formats are set outside the graph (ImGui, raw binds) drew in the pass
		bool _drewFixedFormats = false;
#endif

		SubmitHandle _lastSubmitHandle = {};
		uint32_t _viewMask = 0;

		friend class RenderGraph;
		friend class CTX; // for injection of command buffer data
		friend class PassCmd;
		friend class GraphicsCmd;
		friend class ComputeCmd;
		friend class RayTracingCmd;
		friend class TransferCmd;
	};

	class PassCmd {
	public:
		PassCmd(const PassCmd&) = delete;
		PassCmd& operator=(const PassCmd&) = delete;

		ResolvedTexture texture(GraphTexture texture) const { return _cmd.texture(texture); }

		template<class Struct>
		void cmdPushConstants(const Struct& value, uint32_t offset = 0) {
			static_assert(std::is_trivially_copyable_v<Struct> && !std::is_pointer_v<Struct>, "cmdPushConstant takes struct by value, not a pointer to it");
			_cmd.cmdPushConstants(&value, static_cast<uint32_t>(sizeof(Struct)), offset);
		}
		void cmdPushConstantsEXT(const void* data, uint32_t size, uint32_t offset) { _cmd.cmdPushConstants(data, size, offset); }

		void cmdWriteTimestamp(QueryPoolHandle pool, uint32_t query) { _cmd.cmdWriteTimestamp(pool, query); }
		void cmdWriteTimestamp(const QueryPool& pool, uint32_t query) { _cmd.cmdWriteTimestamp(pool.handle(), query); }

		// valid only inside execute, raw commands must stay inside the accesses the pass declared
		VkCommandBuffer vk();
		VkImage vkImage(GraphTexture texture) const { return _cmd.vkImage(texture); }
		VkBuffer vkBuffer(GraphBuffer buffer) const { return _cmd.vkBuffer(buffer); }
	protected:
		explicit PassCmd(CommandBuffer& cmd) : _cmd(cmd) {}
		CommandBuffer& _cmd;
	};

	class GraphicsCmd final : public PassCmd {
	public:
		void cmdBindGraphicsPipeline(const GraphicsPipeline& pipeline) { _cmd.cmdBindGraphicsPipeline(pipeline.handle()); }
		void cmdBindGraphicsPipeline(GraphicsPipelineHandle handle) { _cmd.cmdBindGraphicsPipeline(handle); }
		void cmdBindDepthState(const DepthState& state) { _cmd.cmdBindDepthState(state); }
		void cmdSetDepthBiasEnable(bool enable) { _cmd.cmdSetDepthBiasEnable(enable); }
		void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp) { _cmd.cmdSetDepthBias(constantFactor, slopeFactor, clamp); }

		void cmdBindIndexBuffer(const Buffer& buffer) { _cmd.cmdBindIndexBuffer(buffer.handle()); }
		void cmdBindIndexBuffer(BufferHandle buffer) { _cmd.cmdBindIndexBuffer(buffer); }

		void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) { _cmd.cmdDraw(vertexCount, instanceCount, firstVertex, firstInstance); }
		void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t baseInstance = 0) { _cmd.cmdDrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, baseInstance); }
		void cmdDrawIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride = 0) { _cmd.cmdDrawIndirect(indirectBuffer, offset, drawCount, stride); }
		void cmdDrawIndexedIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride = 0) { _cmd.cmdDrawIndexedIndirect(indirectBuffer, offset, drawCount, stride); }
		void cmdDrawIndexedIndirectCount(const Buffer& indirectBuffer, VkDeviceSize offset, const Buffer& countBuffer, VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride = 0) {
			_cmd.cmdDrawIndexedIndirectCount(indirectBuffer, offset, countBuffer, countOffset, maxDrawCount, stride);
		}
#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui() { _cmd.cmdDrawImGui(); }
#endif
	private:
		explicit GraphicsCmd(CommandBuffer& cmd) : PassCmd(cmd) {}
		friend class RenderGraph;
	};

	class ComputeCmd final : public PassCmd {
	public:
		void cmdBindComputePipeline(const ComputePipeline& pipeline) { _cmd.cmdBindComputePipeline(pipeline.handle()); }
		void cmdBindComputePipeline(ComputePipelineHandle handle) { _cmd.cmdBindComputePipeline(handle); }

		void cmdDispatchThreadGroup(const Dimensions& threadGroupCount) { _cmd.cmdDispatchThreadGroup(threadGroupCount); }
		void cmdDispatchIndirect(const Buffer& indirectBuffer, VkDeviceSize offset = 0) { _cmd.cmdDispatchIndirect(indirectBuffer, offset); }

		void cmdResetQueryPool(QueryPoolHandle pool, uint32_t first, uint32_t count) { _cmd.cmdResetQueryPool(pool, first, count); }

		template<typename Struct>
		void cmdUpdateBufferEXT(const Buffer& buffer, const Struct& data, size_t bufferOffset = 0) {
			static_assert(sizeof(Struct) <= 65536);
			static_assert(std::is_trivially_copyable_v<Struct>, "cmdUpdateBufferEXT only accepts trivially copyable structs.");
			_cmd.cmdUpdateBufferEXT(buffer.handle(), bufferOffset, sizeof(Struct), &data);
		}
	private:
		explicit ComputeCmd(CommandBuffer& cmd) : PassCmd(cmd) {}
		friend class RenderGraph;
	};

	class RayTracingCmd final : public PassCmd {
	public:
		void cmdBindRayTracingPipeline(const RayTracingPipeline& pipeline) { _cmd.cmdBindRayTracingPipeline(pipeline.handle()); }
		void cmdBindRayTracingPipeline(RayTracingPipelineHandle handle) { _cmd.cmdBindRayTracingPipeline(handle); }

		void cmdTraceRays(const Dimensions& queryDimension) { _cmd.cmdTraceRays(queryDimension); }

		void cmdResetQueryPool(QueryPoolHandle pool, uint32_t first, uint32_t count) { _cmd.cmdResetQueryPool(pool, first, count); }
	private:
		explicit RayTracingCmd(CommandBuffer& cmd) : PassCmd(cmd) {}
		friend class RenderGraph;
	};

	// raw transfer work through vk(), no pipeline should be bound
	class TransferCmd final : public PassCmd {
	public:
		template<class Struct>
		void cmdPushConstants(const Struct&, uint32_t = 0) = delete;
		void cmdPushConstantsEXT(const void*, uint32_t, uint32_t) = delete;
	private:
		explicit TransferCmd(CommandBuffer& cmd) : PassCmd(cmd) {}
		friend class RenderGraph;
	};

} // namespace mythril
