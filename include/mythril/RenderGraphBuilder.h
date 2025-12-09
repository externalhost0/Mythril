//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once


#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <functional>

#include <volk.h>

namespace mythril {
	class CommandBuffer;

	struct ClearColor {
		float r, g, b, a;
		ClearColor(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {};
		VkClearColorValue getAsVkClearColorValue() const {
			return { r, g, b, a };
		}
	};
	struct ClearDepthStencil {
		float depth;
		uint32_t stencil;
		ClearDepthStencil(float depth, uint32_t stencil) : depth(depth), stencil(stencil) {};
		VkClearDepthStencilValue getAsVkClearDepthStencilValue() const {
			return { depth, stencil };
		}
	};
	union ClearValue {
		ClearColor clearColor;
		ClearDepthStencil clearDepthStencil;

		// if the user sets no clear (have no intention of clearing) then do nothing ig
		ClearValue() {};
		ClearValue(float r, float g, float b, float a) : clearColor{r, g, b, a} {};
		ClearValue(float depth, uint32_t stencil) : clearDepthStencil{depth, stencil} {};
	};

	// user never interacts with these
	struct ColorAttachmentInfo {
		VkImageView imageView;
		VkImageLayout imageLayout;
		VkFormat imageFormat;
		// optional resolve target
		VkImageView resolveImageView;
		VkImageLayout resolveImageLayout;

		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;

		VkClearColorValue clearColor;
	};
	struct DepthAttachmentInfo {
		VkImageView imageView;
		VkImageLayout imageLayout;
		VkFormat imageFormat;

		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;

		VkClearDepthStencilValue clearDepthStencil;
	};


	// this is what the user interacts with
	// intermediate struct, PassSource -> PassCompiled
	// totally personal preference if you think working operations should be the default, ie ::CLEAR & ::STORE
	struct WriteSpec {
		InternalTextureHandle texture;
		ClearValue clearValue;
		LoadOperation loadOp = LoadOperation::NO_CARE;
		StoreOperation storeOp = StoreOperation::NO_CARE;
		std::optional<InternalTextureHandle> resolveTexture = std::nullopt;
	};
	struct ReadSpec {
		InternalTextureHandle texture;
		// expectedLayout is always SHADER_READ_ONLY
		VkImageLayout expectedLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	};

	// user defined information from addPass and RenderPassBuilder
	struct PassSource {
		// these three members are sent straight over to PassCompiled
		std::string name;
		enum class Type {
			Graphics,
			Compute,
			General
		} type = Type::Graphics;
		std::function<void(CommandBuffer&)> executeCallback;

		std::vector<WriteSpec> writeOperations;
		std::vector<ReadSpec> readOperations;
	};
	struct CompiledBarrier {
		VkImageMemoryBarrier2 barrier{};
		InternalTextureHandle textureHandle;
	};

	// hidden information that transforms the PassSource into usable info
	struct PassCompiled {
		// transferred info from source
		std::string name;
		PassSource::Type type;
		VkExtent2D extent2D;
		std::function<void(CommandBuffer&)> executeCallback;

		// new info transformed from source on compile()
		std::vector<ColorAttachmentInfo> colorAttachments;
		std::optional<DepthAttachmentInfo> depthAttachment;

		// for automatic image layout transitions
		std::vector<CompiledBarrier> preBarriers;
	};

	// forward declare just for RenderPassBuilder
	class RenderGraph;

	class RenderPassBuilder {
	public:
		RenderPassBuilder() = delete;
		RenderPassBuilder(RenderGraph& graph, std::string name, PassSource::Type type)
		: _graphRef(graph) {
			this->_passSource.name = std::move(name);
			this->_passSource.type = type;
		}

		// currently we only accept textures, should take buffers later and handle them differently
		RenderPassBuilder& write(WriteSpec spec);
		RenderPassBuilder& read(ReadSpec spec);

		// always the last command, must be called for RenderPassBuilder
		void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback);
	private:
		PassSource _passSource;

		RenderGraph& _graphRef;
		friend class RenderGraph;
	};

	class CTX;

	class RenderGraph {
	public:
		inline RenderPassBuilder addPass(const char* name, PassSource::Type type) {
			return RenderPassBuilder{*this, name, type};
		};
		// 2 step,
		// 1. transform passes
		// 2. build pipelines
		void compile(CTX& ctx);
		void execute(CommandBuffer& cmd);
	private:
		// for automatic handling of blitting to swapchian
		InternalTextureHandle _lastColorTexture;

		std::vector<PassSource> _sourcePasses;
		std::vector<PassCompiled> _compiledPasses;

		bool _hasCompiled = false;

		friend class RenderPassBuilder;
	};
}