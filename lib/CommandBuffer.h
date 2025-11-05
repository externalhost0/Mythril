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
		void cmdBindRenderPipeline(InternalPipelineHandle handle);
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
		void cmdDrawIndirect();
		void cmdDrawIndexedIndirect();

		void cmdTransitionLayout(InternalTextureHandle source, VkImageLayout newLayout);
		void cmdCopyImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdCopyImageToBuffer(InternalTextureHandle source, InternalBufferHandle destination, const VkBufferImageCopy& region);

		// plugins
#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui();
#endif
	private:
		void cmdBeginRendering();
		void cmdEndRendering();

		void cmdBindDepthState(const DepthState& state);
		void cmdSetDepthBiasEnable(bool enable);
		void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp);

		void cmdTransitionLayout(InternalTextureHandle source, VkImageLayout currentLayout, VkImageLayout newLayout);
		void cmdTransitionSwapchainLayout(VkImageLayout newLayout);

		void cmdBlitToSwapchain(InternalTextureHandle source);
		void cmdPrepareToSwapchain(InternalTextureHandle source);

		void _cmdCopyImage(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D size);
		void _cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination, VkExtent2D srcSize, VkExtent2D dstSize);
		void _bufferBarrier(InternalBufferHandle bufhandle, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage);

		void _cmdSetViewport(VkExtent2D extent2D);
		void _cmdSetScissor(VkExtent2D extent2D);

	private:
		CTX* _ctx = nullptr;
		const ImmediateCommands::CommandBufferWrapper* _wrapper = nullptr;

		bool _isRendering = false; // cmdBeginRendering

		VkPipeline _lastBoundvkPipeline = VK_NULL_HANDLE;
		InternalPipelineHandle _currentPipelineHandle;
		PassCompiled _activePass;

		SubmitHandle _lastSubmitHandle = {};
		Type _cmdType;

		friend class RenderGraph; // for access to set _activePass
		friend class CTX; // for injection of command buffer data
	};

}
