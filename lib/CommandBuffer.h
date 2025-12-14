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
	struct PipelineCommon;

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

	// https://github.com/corporateshark/lightweightvk/blob/f5598737c2179e329e519e1fe094ade1cafbc97c/lvk/LVK.h#L315
	struct Dimensions {
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		inline Dimensions divide1D(uint32_t v) const {
			return {.width = width / v, .height = height, .depth = depth};
		}
		inline Dimensions divide2D(uint32_t v) const {
			return {.width = width / v, .height = height / v, .depth = depth};
		}
		inline Dimensions divide3D(uint32_t v) const {
			return {.width = width / v, .height = height / v, .depth = depth / v};
		}
		inline bool operator==(const Dimensions& other) const {
			return width == other.width && height == other.height && depth == other.depth;
		}
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
		inline bool isDrying() const { return _isDryRun; };
	public:
		// all possible commands user can call inside setExecuteCallback
		// all commands in this section NEED to detect if they are being called while in a dryRun
		// most commands will include: if (_isDryRun) return;

		// ALL BELOW COMMANDS HAVE SPECIAL BEHAVIOR ON DRYRUN //

		void cmdBindComputePipeline(InternalComputePipelineHandle handle);
		void cmdBindGraphicsPipeline(InternalGraphicsPipelineHandle handle);
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

		void cmdDispatchThreadGroup(const Dimensions& threadGroupCount);

		void cmdTransitionLayout(InternalTextureHandle source, VkImageLayout newLayout);
		void cmdCopyImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdBlitImage(InternalTextureHandle source, InternalTextureHandle destination);
		void cmdCopyImageToBuffer(InternalTextureHandle source, InternalBufferHandle destination, const VkBufferImageCopy& region);

		// plugins
#ifdef MYTH_ENABLED_IMGUI
		void cmdDrawImGui();
#endif
	private:
		// just repeated logic
		void cmdBindPipelineImpl(const PipelineCommon* common, VkPipelineBindPoint bindPoint);

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

		// helpers
		PassSource::Type getCurrentPassType();
	private:
		// pretty important members for communication to the rest of the renderer
		CTX* _ctx = nullptr;
		const ImmediateCommands::CommandBufferWrapper* _wrapper = nullptr;

		// all set via RenderGraph
		VkPipeline _lastBoundvkPipeline = VK_NULL_HANDLE;

		// avoid lookup and store the common data
		PipelineCommon* _currentPipelineCommon = nullptr;
		std::variant<InternalGraphicsPipelineHandle, InternalComputePipelineHandle> _currentPipelineHandle;
		PassCompiled _activePass;

		bool _isRendering = false; // cmdBeginRendering

		SubmitHandle _lastSubmitHandle = {};
		Type _cmdType = Type::General;

		bool _isDryRun = true; // for dummy CommandBuffer

		friend class RenderGraph; // for access to set _activePass
		friend class CTX; // for injection of command buffer data
	};

}
