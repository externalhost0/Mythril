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

namespace mythril {
	// forward declarations
	class CTX;
	class RenderPass;

	// we only use it for the cmdBeginRendering command anyways
	struct Dependencies {
		enum { kMaxSubmitDependencies = 4 };
		InternalTextureHandle textures[kMaxSubmitDependencies] = {};
		InternalBufferHandle buffers[kMaxSubmitDependencies] = {};
	};

	struct DepthState {
		CompareOperation compareOp = CompareOperation::CompareOp_AlwaysPass;
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
	public:
		// all possible commands user can call inside setExecuteCallback
		// all commands in this section NEED to detect if they are being called while in a dryRun
		// most commands will include: if (_isDryRun) return;

		// ALL BELOW COMMANDS HAVE SPECIAL BEHAVIOR ON DRYRUN //

		void cmdBindRenderPipeline(InternalGraphicsPipelineHandle handle);
		void cmdBindDepthState(const DepthState& state);

		// ALL BELOW COMMANDS SHOULD RETURN ON DRYRUN //
		void cmdBindIndexBuffer(InternalBufferHandle buffer);
		// we can pass in structs of any type for push constants!!
		// make sure it is mirrored on the shader code
		void cmdPushConstants(const void* data, uint32_t size, uint32_t offset);
		template<class Struct>
		void cmdPushConstants(const Struct& type, uint32_t offset = 0) {
			cmdPushConstants(&type, (uint32_t)sizeof(Struct), offset);
		}
		void cmdUpdateBuffer(InternalBufferHandle handle, size_t offset, size_t size, const void* data);
		template<typename Struct>
		void cmdUpdateBuffer(InternalBufferHandle handle, const Struct& data, size_t bufferOffset = 0) {
			cmdUpdateBuffer(handle, bufferOffset, sizeof(Struct), &data);
		}
		void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
		void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t baseInstance = 0);

		void cmdTransitionLayout(InternalTextureHandle source, VkImageLayout newLayout);
		void cmdCopyImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdCopyImageToBuffer(InternalTextureHandle source, InternalBufferHandle destination, const VkBufferImageCopy& region);

		// plugins
#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui();
#endif
	private:
		// all functions that still have equivalent Vulkan commands but should be abstracted away from user
		void cmdBeginRenderingImpl();
		void cmdEndRenderingImpl();

		void cmdSetDepthBiasEnableImpl(bool enable);
		void cmdSetDepthBiasImpl(float constantFactor, float slopeFactor, float clamp);

		void cmdTransitionLayoutImpl(InternalTextureHandle source, VkImageLayout currentLayout, VkImageLayout newLayout);
		void cmdTransitionSwapchainLayoutImpl(VkImageLayout newLayout);

		void cmdBlitToSwapchainImpl(InternalTextureHandle source);
		void cmdPrepareToSwapchainImpl(InternalTextureHandle source);

		void cmdCopyImageImpl(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D size);
		void cmdBlitImageImpl(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize);

		void cmdSetViewportImpl(VkExtent2D extent2D);
		void cmdSetScissorImpl(VkExtent2D extent2D);

		void bufferBarrierImpl(InternalBufferHandle bufhandle, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage);
	private:
		// pretty important members for communication to the rest of the renderer
		CTX* _ctx = nullptr;
		const ImmediateCommands::CommandBufferWrapper* _wrapper = nullptr;

		// all set via RenderGraph
		VkPipeline _lastBoundvkPipeline = VK_NULL_HANDLE;
		InternalGraphicsPipelineHandle _currentPipelineHandle;
		PassCompiled _activePass;

		bool _isRendering = false; // cmdBeginRendering

		SubmitHandle _lastSubmitHandle = {};
		Type _cmdType = Type::General;

		bool _isDryRun = true; // for dummy CommandBuffer

		friend class RenderGraph; // for access to set _activePass
		friend class CTX; // for injection of command buffer data
	};

}
