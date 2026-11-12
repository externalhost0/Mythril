//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "Objects.h"
#include "ObjectHandles.h"
#include "RenderGraphDescriptions.h"

#include <volk.h>

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mythril {
    class RenderGraph;


    // GPU pipeline stage/access pair tracked per buffer in the render graph.
    struct StageAccess {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 access;
    };

    struct AttachmentInfo;
    struct SubresourceRange;
    struct TextureAccessCache;
    namespace vkutil { struct BufferAccessTracker; }
    struct CompiledImageBarrier;
    struct CompiledBufferBarrier;
    struct UploadData;
    struct CompiledPass;
    struct PassMember;
    struct TextureShape;
    struct GraphResource;

    struct BasePassBuilder {
        BasePassBuilder() = delete;
        BasePassBuilder(RenderGraph& rGraph, const char* pName, PassDesc::Type type);
    protected:
        RenderGraph& _rGraph;
        void setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback);
    private:
        PassDesc _passSource;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class IntermediateBuilder;
    	friend class RayTracingPassBuilder;
    };


    inline void add(PassDesc& passSource, const TextureDesc& desc, const TextureAccess access) {
        passSource.textureDependencyOperations.emplace_back(desc, access);
    }
    inline void add(PassDesc& passSource, Buffer& buffer, const BufferAccess access) {
        passSource.bufferDependencyOperations.push_back({buffer, access});
    }
    inline void add(PassDesc& passSource, const AttachmentDesc& desc) {
        passSource.attachmentOperations.push_back(desc);
    }

    class GraphicsPassBuilder {
    public:
        GraphicsPassBuilder() = delete;
        GraphicsPassBuilder(RenderGraph& rGraph, const char* pName)
    	: base(rGraph, pName, PassDesc::Type::Graphics) {}
#ifdef DEBUG
        ~GraphicsPassBuilder();
#endif

        [[nodiscard]] GraphicsPassBuilder& attachment(const AttachmentDesc& desc) {
            add(this->base._passSource, desc);
            return *this;
        }
    	// dependency for texture
        [[nodiscard]] GraphicsPassBuilder& dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::Sampled) {
            add(this->base._passSource, texDesc, access);
            return *this;
        }
        [[nodiscard]] GraphicsPassBuilder& dependency(Buffer& buffer, const BufferAccess access) {
            add(this->base._passSource, buffer, access);
            return *this;
        }
    	[[nodiscard]] GraphicsPassBuilder& dependency(const std::span<const GraphTexture> textures, const TextureAccess access = TextureAccess::Sampled) {
            for (const GraphTexture tex : textures)
                add(this->base._passSource, tex, access);
	        return *this;
        }
        [[nodiscard]] GraphicsPassBuilder& attachments(const std::span<const AttachmentDesc> descs) {
            for (const auto& desc : descs)
                add(this->base._passSource, desc);
            return *this;
        }
		// conditionally perform a renderpass without having to enter the execution stage
        [[nodiscard]] GraphicsPassBuilder& condition(std::function<bool()> fn) {
            this->base._passSource.conditionCallback = std::move(fn);
            return *this;
        }
    	[[nodiscard]] GraphicsPassBuilder& multiview(uint32_t layerCount, uint32_t viewMask) {
            this->base._passSource.layerCount = layerCount;
            this->base._passSource.viewMask = viewMask;
	        return *this;
        }

        void execute(const std::function<void(CommandBuffer& cmd)>& callback) {
            base.setExecuteCallback(callback);
        }
    private:
        BasePassBuilder base;
    };

    class ComputePassBuilder {
    public:
        ComputePassBuilder() = delete;
        ComputePassBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Compute) {}
#ifdef DEBUG
        ~ComputePassBuilder();
#endif

        [[nodiscard]] ComputePassBuilder& dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::StorageReadWrite) {
                add(this->base._passSource, texDesc, access);
            return *this;
        }
        [[nodiscard]] ComputePassBuilder& dependency(Buffer& buffer, const BufferAccess access) {
            add(this->base._passSource, buffer, access);
            return *this;
        }
        [[nodiscard]] ComputePassBuilder& dependency(const std::span<const GraphTexture> textures, const TextureAccess access = TextureAccess::StorageReadWrite) {
            for (const GraphTexture tex : textures)
                add(this->base._passSource, tex, access);
            return *this;
        }

    	// dispatches in this pass are unordered unless requested, use when one dispatch reads what a previous one wrote (ie: mip chain)
    	// has a performance overhead so only use if the commands in your pass need to be dependent on previous
    	[[nodiscard]] ComputePassBuilder& executeCmdsInOrder() {
        	this->base._passSource.executeInOrder = true;
	        return *this;
        }
    	[[nodiscard]] ComputePassBuilder& async() {
        	this->base._passSource.queue = QueueAffinity::AsyncCompute;
	        return *this;
        }

        void execute(const std::function<void(CommandBuffer& cmd)>& callback) {
            base.setExecuteCallback(callback);
        }
    private:
        BasePassBuilder base;
    };

	class RayTracingPassBuilder {
	public:
		RayTracingPassBuilder() = delete;
		RayTracingPassBuilder(RenderGraph& rGraph, const char* pName)
			: base(rGraph, pName, PassDesc::Type::RayTracing) {}
#ifdef DEBUG
		~RayTracingPassBuilder();
#endif

		[[nodiscard]] RayTracingPassBuilder& dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::StorageReadWrite) {
			add(this->base._passSource, texDesc, access);
			return *this;
		}
		[[nodiscard]] RayTracingPassBuilder& dependency(Buffer& buffer, const BufferAccess access) {
			add(this->base._passSource, buffer, access);
			return *this;
		}
		[[nodiscard]] RayTracingPassBuilder& dependency(const std::span<const GraphTexture> textures, const TextureAccess access = TextureAccess::StorageReadWrite) {
			for (const GraphTexture tex : textures)
				add(this->base._passSource, tex, access);
			return *this;
		}
		[[nodiscard]] RayTracingPassBuilder& executeCmdsInOrder() {
			this->base._passSource.executeInOrder = true;
			return *this;
		}

		void execute(const std::function<void(CommandBuffer& cmd)>& callback) {
			base.setExecuteCallback(callback);
		}
	private:
		BasePassBuilder base;
	};

    class IntermediateBuilder {
    public:
        IntermediateBuilder() = delete;
        IntermediateBuilder(RenderGraph& rGraph, const char* pName)
            : base(rGraph, pName, PassDesc::Type::Intermediate) {}

        // helper that encomposses a order of the below fuctions
        IntermediateBuilder& copy(TextureDesc src, TextureDesc dst);
        IntermediateBuilder& blit(TextureDesc src, TextureDesc dst);
        //IntermediateBuilder& generateMipmaps(const Texture& texture);
        //IntermediateBuilder& dependency(Buffer& buffer, BufferAccess access);
        IntermediateBuilder& update(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset = 0);
        IntermediateBuilder& upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset = 0);
        void finish() const;
    private:
        BasePassBuilder base;
    };

	// placeholder for Phase 3 timeline-semaphore cross-queue sync data
	struct CrossQueueSync {};

	struct ExecutionSchedule {
		// maps a compiled pass index to its depth level: passDepth[passIdx] = depth
		std::unordered_map<uint32_t, uint32_t> pass_depths;
		// groups pass indices together by layer, indices are of _compiledPasses:
		// layers[0] = { passA, passB }
		std::vector<std::vector<uint32_t>> layers;
	};
    class RenderGraph {
    public:
        RenderGraph();
        ~RenderGraph();
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) noexcept;
        RenderGraph& operator=(RenderGraph&&) noexcept;

        // setup steps are designed with minimal overhead
        // hence why we simply take directly the data the user gives and stores it, nothing else
        [[nodiscard]] GraphicsPassBuilder addGraphicsPass(const char* pName) {
            return GraphicsPassBuilder{*this, pName};
        }
        [[nodiscard]] ComputePassBuilder addComputePass(const char* pName) {
            return ComputePassBuilder{*this, pName};
        }
        [[nodiscard]] IntermediateBuilder addIntermediate(const char* pName) {
            return IntermediateBuilder{*this, pName};
        }
    	[[nodiscard]] RayTracingPassBuilder addRayTracingPass(const char* pName) {
	        return RayTracingPassBuilder{*this, pName};
        }
        // compile is where ALL of the overhead should be, all data stored during setup is now translated into pieces that are easier to direct with
        // called sparingly, perhaps once
        void compile(CTX& rCtx);
        // called every frame
    	void executeEXT(CTX& rCtx);

    	void execute(CommandBuffer& cmd);

        // owned by the graph, created at compile with the usage its surviving passes need
        [[nodiscard]] GraphTexture createTexture(GraphTextureSpec spec);
        // makes a created texture readable outside the graph (ImGui), its last writer is never culled and leaves it shader readable
        void exportTexture(GraphTexture texture);
        // valid after compile
        [[nodiscard]] TextureHandle exported(GraphTexture texture) const;
        // non-owning, the caller keeps the texture alive for the graph's lifetime, importing the same texture twice returns the same id
        [[nodiscard]] GraphTexture importTexture(const Texture& texture);
        // whichever swapchain image is current when the graph records
        [[nodiscard]] GraphTexture backBuffer();

        void trackWindowSized(Texture& texture);
        void trackWindowSized(Texture& texture, std::function<Dimensions(Dimensions)> scaleFn);
        void resizeTrackedWindowSized(const Dimensions& swapchainDimensions) const;
    private:
        ResolvedTexture resolve(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, GraphTexture texture) const;
        void PerformBarrierTransitions(CommandBuffer& cmd, std::span<const CompiledImageBarrier> imageBarriers, std::span<const CompiledBufferBarrier> bufferBarriers);
        void realizeDeclared(CTX& rCtx, const std::vector<uint32_t>& culledOrder);
        void RecordIntermediateOps(CommandBuffer& cmd, const CompiledPass& pass) const;
        TextureShape shapeOf(const CTX& rCtx, GraphTexture texture) const;
        void planResourceAccess(const CTX& rCtx, const TextureDesc& texDesc, VkImageLayout desiredLayout, const StageAccess& dstMask, CompiledPass& outPass, const PassDesc& pass) const;
        void planPassResources(const CTX& rCtx, const PassDesc& passDesc, CompiledPass& outPass) const;
        void planAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) const;
        void bindPass(CTX& rCtx, CompiledPass& pass) const;
        void performDryRun(CTX& rCtx);
        std::unordered_map<TextureHandle, TextureAccessCache> _resourceTrackers;
        std::unordered_map<BufferHandle, vkutil::BufferAccessTracker> _bufferTrackers;
        struct WindowSizedTexture {
            Texture* texture = nullptr;
            std::function<Dimensions(Dimensions)> scaleFn;
        };
        std::vector<WindowSizedTexture> _windowSizedTextures;
        std::vector<GraphResource> _textures;
        std::vector<PassDesc> _passDescriptions;
        std::vector<CompiledPass> _compiledPasses;

        bool _hasCompiled = false;
        // epoch snapshot from CTX at last compile(). execute() auto-recompiles on mismatch.
        uint64_t _compiledEpoch = 0;

    	ExecutionSchedule _schedule;
    	// per-layer, per-queue: which compiled-pass indices to record
    	std::vector<std::array<std::vector<uint32_t>, kQueueCount>> _layerByQueue;
    	// per-layer-boundary: which queues must wait on which producer queues' timelines
    	std::vector<CrossQueueSync> _layerSyncs;

        friend struct BasePassBuilder;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class IntermediateBuilder;
    	friend class RayTracingPassBuilder;
        friend class CommandBuffer;
    };
}
