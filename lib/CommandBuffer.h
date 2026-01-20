//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include "vkenums.h"
#include "ObjectHandles.h"
#include "ImmediateCommands.h"
#include "../include/mythril/RenderGraphBuilder.h"

#include <volk.h>

#include <span>

#include "Pipelines.h"

namespace mythril {
	// forward declarations
	class CTX;
	class AllocatedTexture;
	struct PipelineCoreData;

	// we only use it for the cmdBeginRendering command anyways
	struct Dependencies {
		enum { kMaxSubmitDependencies = 4 };
		TextureHandle textures[kMaxSubmitDependencies] = {};
		BufferHandle buffers[kMaxSubmitDependencies] = {};
	};

	struct DepthState {
		CompareOp compareOp = CompareOp::Always;
		bool isDepthWriteEnabled = false;
	};

	// lets RAII this guy
	class CommandBuffer final {
	public:
		enum class Type {
			General = 0,
			Graphics,
			Compute,
		};
		CommandBuffer() = default;
		explicit CommandBuffer(CTX* ctx, Type type);
		~CommandBuffer();

		VkCommandBufferSubmitInfo requestSubmitInfo() const;
		bool isDrying() const { return _isDryRun; }
	public:
		// all possible commands user can call inside setExecuteCallback
		// all commands in this section NEED to detect if they are being called while in a dryRun
		// most commands will include: if (_isDryRun) return;

		// ALL BELOW COMMANDS HAVE SPECIAL BEHAVIOR ON DRYRUN //

		void cmdBindComputePipeline(const ComputePipeline& computePipeline) { cmdBindComputePipeline(computePipeline.handle()); }
		void cmdBindGraphicsPipeline(const GraphicsPipeline& graphicsPipeline) { cmdBindGraphicsPipeline(graphicsPipeline.handle()); }
		void cmdBindComputePipeline(ComputePipelineHandle handle);
		void cmdBindGraphicsPipeline(GraphicsPipelineHandle handle);
		// ALL BELOW COMMANDS SHOULD RETURN ON DRYRUN //
		void cmdBindDepthState(const DepthState& state);
		void cmdSetDepthBiasEnable(bool enable);
		void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp);

		void cmdBeginRendering(uint32_t layers = 1);
		void cmdEndRendering();

		void cmdDispatchThreadGroup(const Dimensions& threadGroupCount);


		void cmdBindIndexBuffer(const Buffer& buffer) { cmdBindIndexBuffer(buffer.handle()); }
		void cmdBindIndexBuffer(BufferHandle buffer);
		// we can pass in structs of any type for push constants!!
		// make sure it is mirrored on the shader code
		void cmdPushConstants(const void* data, uint32_t size, uint32_t offset);
		template<class Struct>
		void cmdPushConstants(const Struct& type, uint32_t offset = 0) {
			cmdPushConstants(&type, (uint32_t)sizeof(Struct), offset);
		}

		template<typename T>
		void cmdUpdateBuffer(const Buffer& buffer, const std::vector<T>& data, size_t bufferOffset = 0) {
			static_assert(std::is_trivially_copyable_v<T>, "Vector element type must be trivially copyable.");
			cmdUpdateBuffer(buffer, bufferOffset, data.size() * sizeof(T), data.data());
		}

		void cmdUpdateBuffer(const Buffer& buffer, size_t offset, size_t size, const void* data) {
			cmdUpdateBuffer(buffer.handle(), offset, size, data);
		}
		template<typename Struct>
		void cmdUpdateBuffer(const Buffer& buffer, const Struct& data, size_t bufferOffset = 0) {
			static_assert(sizeof(Struct) <= 65536);
			static_assert(std::is_trivially_copyable_v<Struct>, "cmdUpdateBuffer template only accepts trivially copyable structs.");
			cmdUpdateBuffer(buffer, bufferOffset, sizeof(Struct), &data);
		}
		// void these two
		void cmdUpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data);
		template<typename Struct>
		void cmdUpdateBuffer(BufferHandle handle, const Struct& data, size_t bufferOffset = 0) {
			cmdUpdateBuffer(handle, bufferOffset, sizeof(Struct), &data);
		}

		void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
		void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t baseInstance = 0);
		void cmdDrawIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride = 0);
		void cmdDrawIndexedIndirect(const Buffer& indirectBuffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride = 0);

		void cmdGenerateMipmap(TextureHandle handle);
		void cmdGenerateMipmap(const Texture& texture) { cmdGenerateMipmap(texture.handle()); }

		void cmdTransitionLayout(const Texture &source, VkImageLayout newLayout) { cmdTransitionLayout(source.handle(), newLayout);}
		void cmdCopyImage(const Texture& source, const Texture& destination) { cmdCopyImage(source.handle(), destination.handle()); }
		void cmdBlitImage(const Texture& source, const Texture& destination) { cmdBlitImage(source.handle(), destination.handle()); }
		void cmdCopyImageToBuffer(const Texture& source, const Buffer& destination, const VkBufferImageCopy& region) {
			cmdCopyImageToBuffer(source.handle(), destination.handle(), region);
		}

		void cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout, VkImageSubresourceRange range);
		void cmdTransitionLayout(TextureHandle source, VkImageLayout newLayout);
		void cmdCopyImage(TextureHandle source, TextureHandle destination);
		void cmdBlitImage(TextureHandle source, TextureHandle destination);
		void cmdCopyImageToBuffer(TextureHandle source, BufferHandle destination, const VkBufferImageCopy& region);

		void cmdClearColorImage(TextureHandle texture, const ClearColor& value);
		void cmdClearDepthStencilImage(TextureHandle texture, const ClearDepthStencil& value);

		// plugins
#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui();
#endif
	private:
		// just repeated logic
		void cmdBindPipelineImpl(const PipelineCoreData* common, VkPipelineBindPoint bindPoint);

		// all functions that still have equivalent Vulkan commands but should be abstracted away from user
		void cmdBeginRenderingImpl(uint32_t layers);
		void cmdEndRenderingImpl();

		void cmdTransitionLayoutImpl(TextureHandle source, VkImageLayout currentLayout, VkImageLayout newLayout, VkImageSubresourceRange range);

		void cmdCopyImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D size);
		void cmdBlitImageImpl(TextureHandle source, TextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize);

		void cmdSetViewportImpl(VkExtent2D extent2D);
		void cmdSetScissorImpl(VkExtent2D extent2D);

		void bufferBarrierImpl(BufferHandle bufhandle, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage);

		// helpers
		PassSource::Type getCurrentPassType();
		void CheckTextureRenderingUsage(const AllocatedTexture& source, const AllocatedTexture& destination, const char* operation);
		void CheckImageLayoutAuto(TextureHandle sourceHandle, TextureHandle destinationHandle, const char* operation);
	private:
		// pretty important members for communication to the rest of the renderer
		CTX* _ctx = nullptr;
		const ImmediateCommands::CommandBufferWrapper* _wrapper = nullptr;

		// all set via RenderGraph
		VkPipeline _lastBoundvkPipeline = VK_NULL_HANDLE;

		// avoid lookup and store the common data
		SharedPipelineInfo* _currentPipelineInfo = nullptr;
		std::variant<GraphicsPipelineHandle, ComputePipelineHandle> _currentPipelineHandle;
		PassCompiled _activePass;

		bool _isRendering = false; // cmdBeginRendering

		SubmitHandle _lastSubmitHandle = {};
		Type _cmdType = Type::General;

		bool _isDryRun = true; // for dummy CommandBuffer

		friend class RenderGraph; // for access to set _activePass
		friend class CTX; // for injection of command buffer data
	};

}
