//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "../../lib/ObjectHandles.h"
#include "../../lib/vkenums.h"


#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <functional>
#include <utility>

#include <volk.h>

namespace mythril {
	class CommandBuffer;

	struct ClearColor {
		float r, g, b, a;
		ClearColor(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {};
		VkClearColorValue getAsVkClearColorValue() const {
			return { {r, g, b, a} };
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
		// im leaving this exposed for end-user but im not really sure what other layouts they want, maybe GENERAL
		VkImageLayout expectedLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	};

	// user defined information from addPass and RenderPassBuilder
	struct PassSource {
		enum class Type { Graphics, Compute };

		PassSource(const char* name, Type type)
		: name(name), type(type) {}

		// these three members are sent straight over to PassCompiled
		std::string name;
		Type type;
		std::function<void(CommandBuffer&)> executeCallback{};

		std::vector<WriteSpec> writeOperations{}; // empty for compute
		std::vector<ReadSpec> readOperations{};
	};
	struct CompiledBarrier {
		VkImageMemoryBarrier2 barrier{};
		InternalTextureHandle textureHandle;
		bool isRead = false;
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

	class IPassBuilder {
	public:
		IPassBuilder() = delete;
		virtual ~IPassBuilder() = default;
		IPassBuilder(RenderGraph& graphRef, const char* name, const PassSource::Type type)
		: _graphRef(graphRef), _passSource(name, type) {
			assert(!_passSource.name.empty());
			assert(_passSource.type == type);
		};
		virtual void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback) = 0;
	protected:
		RenderGraph& _graphRef;
		PassSource _passSource;
	};

	class GraphicsPassBuilder : public IPassBuilder {
	public:
		GraphicsPassBuilder(RenderGraph& graphRef, const char* name) : IPassBuilder(graphRef, name, PassSource::Type::Graphics) {}

		GraphicsPassBuilder& write(const WriteSpec& spec);
		GraphicsPassBuilder& read(const ReadSpec& spec);
		void setExecuteCallback(const std::function<void (CommandBuffer &)>& callback) override;

		friend class RenderGraph;
	};
	class ComputePassBuilder : public IPassBuilder {
	public:
		ComputePassBuilder(RenderGraph& graphRef, const char* name) : IPassBuilder(graphRef, name, PassSource::Type::Compute) {};
		// compute passes dont write to textures like renderpasses so we dont offer a write operation
		ComputePassBuilder& read(const ReadSpec& spec);
		void setExecuteCallback(const std::function<void (CommandBuffer &)>& callback) override;

		friend class RenderGraph;
	};

	class CTX;

	class RenderGraph {
	public:
		ComputePassBuilder addComputePass(const char* name) {
			return ComputePassBuilder{*this, name};
		}
		GraphicsPassBuilder addGraphicsPass(const char* name) {
			return GraphicsPassBuilder{*this, name};
		}
		// 2 step,
		// 1. transform passes
		// 2. build pipelines
		void compile(CTX& ctx);
		void execute(CommandBuffer& cmd);
	private:
		std::unordered_map<InternalTextureHandle, VkImageLayout> _initialLayoutsPerFrame{};
		std::vector<PassSource> _sourcePasses;
		std::vector<PassCompiled> _compiledPasses;

		bool _hasCompiled = false;

		friend class RenderPassBuilder;
		friend class GraphicsPassBuilder;
		friend class ComputePassBuilder;
	};
}