//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "Objects.h"
#include "ObjectHandles.h"
#include "RenderGraphDescriptions.h"
#include "SubmitHandle.h"

#include <volk.h>

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <ranges>
#include <type_traits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace mythril {
    class RenderGraph;
    class GraphicsCmd;
    class ComputeCmd;
    class RayTracingCmd;
    class TransferCmd;


    // GPU pipeline stage/access pair tracked per buffer in the render graph.
    struct StageAccess {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 access;
    };

    struct AttachmentInfo;
    struct SubresourceRange;
    struct TextureAccessCache;
    namespace vkutil {
        struct BufferAccessTracker;
    }
    namespace detail {
        class AllocatedTexture;
    }
    struct CompiledImageBarrier;
    struct CompiledBufferBarrier;
    struct CompiledPass;
    struct PassMember;
    struct TextureShape;
    struct GraphResource;
    struct PassUses;
    struct GraphBufferResource;
    struct PushResourceField;
    struct OwnershipTransfer;
    struct ExternalUse;
    class ImmediateCommands;


    struct TransientKey {
        Dimensions dimensions{};
        TextureType type{};
        VkFormat format{};
        SampleCount samples{};
        uint32_t numMips{};
        uint32_t numLayers{};
        ComponentMapping components{};
        TextureUsageBits usage{};
        StorageType storage{};
        bool concurrent{};
        bool operator==(const TransientKey&) const = default;
    };
    inline constexpr uint64_t kPoolIdleFrames = 8;
    struct TransientPool {
        struct Entry {
            TransientKey key;
            Texture texture;
            uint64_t lastUsedFrame = 0;
            bool taken = false;
        };
        std::vector<Entry> entries;

        void beginCompile() {
            for (Entry& e: entries)
                e.taken = false;
        }
        TextureHandle acquire(CTX& ctx, const TransientKey& key, TextureHandle previous, const TextureSpec& spec);

        void evictIdle(uint64_t frame) {
            std::erase_if(entries, [frame](const Entry& e) {
                return !e.taken && frame - e.lastUsedFrame > kPoolIdleFrames;
            });
        }
    };


    class PassArena {
    public:
        PassArena() = default;
        ~PassArena() { clear(); }
        PassArena(const PassArena&) = delete;
        PassArena& operator=(const PassArena&) = delete;
        PassArena(PassArena&&) noexcept = default;
        PassArena& operator=(PassArena&&) noexcept = default;

        template<typename T>
        T& create() {
            T* object = ::new (allocate(sizeof(T), alignof(T))) T{};
            if constexpr (!std::is_trivially_destructible_v<T>)
                _destructors.push_back({ object, [](void* o) {
                                            static_cast<T*>(o)->~T();
                                        } });
            return *object;
        }
        std::span<const std::byte> copy(std::span<const std::byte> bytes);
        void clear();
    private:
        void* allocate(size_t size, size_t align);
        struct Block {
            std::unique_ptr<std::byte[]> bytes;
            size_t size = 0;
        };
        struct Destructor {
            void* object;
            void (*destroy)(void*);
        };
        std::vector<Block> _blocks;
        size_t _block = 0;
        size_t _offset = 0;
        std::vector<Destructor> _destructors;
    };

    struct NoPassData {};

    struct BasePassBuilder {
        BasePassBuilder() = delete;
        BasePassBuilder(RenderGraph& rGraph, const char* pName, PassDesc::Type type);
        // a write returns the version this pass produces, a read returns the version it names
        GraphTexture dependency(const TextureDesc& texDesc, TextureAccess access);
        GraphBuffer dependency(GraphBuffer buffer, BufferAccess access);
        GraphAccelerationStructure dependency(GraphAccelerationStructure structure, AccelerationStructureAccess access);
        // asserts the access is a read, a write needs a mutable array to take the new versions
        void readDependencies(std::span<const GraphTexture> textures, TextureAccess access);
        // the attachment texture becomes the version this pass writes
        void attachment(GraphTexture& texture, const AttachmentOptions& options);
        // texture and resolve both become the versions this pass writes
        void attachment(GraphTexture& texture, GraphTexture& resolve, const AttachmentOptions& options);
    private:
        GraphBuffer declare(GraphBuffer buffer, BufferAccess access);
        // a read the pass already declared with the same access is not declared again
        GraphBuffer declareOnce(GraphBuffer buffer, BufferAccess access);
        RenderGraph& _rGraph;
        PassDesc _passSource;
        friend class RenderGraph;
        friend class GraphicsPassBuilder;
        friend class ComputePassBuilder;
        friend class TransferBuilder;
        friend class RayTracingPassBuilder;
    };

    class GraphicsPassBuilder {
    public:
        GraphicsPassBuilder() = delete;

        void attachment(GraphTexture& texture, const AttachmentOptions& options) {
            base.attachment(texture, options);
        }
        void attachment(GraphTexture& texture, GraphTexture& resolve, const AttachmentOptions& options) {
            base.attachment(texture, resolve, options);
        }
        GraphTexture dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::Sampled) {
            return base.dependency(texDesc, access);
        }
        // each texture becomes the version this pass names or produces
        void dependency(const std::span<GraphTexture> textures, const TextureAccess access = TextureAccess::Sampled) {
            for (GraphTexture& texture: textures)
                texture = base.dependency(texture, access);
        }
        // const textures only take reads and keep their versions
        template<std::ranges::contiguous_range Range> requires std::same_as<std::ranges::range_reference_t<Range>, const GraphTexture&>
        void dependency(Range&& textures, const TextureAccess access = TextureAccess::Sampled) {
            base.readDependencies(std::span<const GraphTexture>(textures), access);
        }
        GraphBuffer dependency(const GraphBuffer buffer, const BufferAccess access) {
            return base.dependency(buffer, access);
        }
        GraphAccelerationStructure dependency(const GraphAccelerationStructure structure, const AccelerationStructureAccess access = AccelerationStructureAccess::Read) {
            return base.dependency(structure, access);
        }
        void multiview(uint32_t layerCount, uint32_t viewMask) {
            this->base._passSource.layerCount = layerCount;
            this->base._passSource.viewMask = viewMask;
        }
        // keeps the pass when nothing the graph sees reads its output (queries, bindless writes, debug printf)
        void sideEffect() {
            this->base._passSource.sideEffect = true;
        }
    private:
        GraphicsPassBuilder(RenderGraph& rGraph, const char* pName) :
            base(rGraph, pName, PassDesc::Type::Graphics) {}
        BasePassBuilder base;
        friend class RenderGraph;
    };

    class ComputePassBuilder {
    public:
        ComputePassBuilder() = delete;

        GraphTexture dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::StorageReadWrite) {
            return base.dependency(texDesc, access);
        }
        // each texture becomes the version this pass names or produces
        void dependency(const std::span<GraphTexture> textures, const TextureAccess access = TextureAccess::StorageReadWrite) {
            for (GraphTexture& texture: textures)
                texture = base.dependency(texture, access);
        }
        // const textures only take reads and keep their versions
        template<std::ranges::contiguous_range Range> requires std::same_as<std::ranges::range_reference_t<Range>, const GraphTexture&>
        void dependency(Range&& textures, const TextureAccess access = TextureAccess::StorageRead) {
            base.readDependencies(std::span<const GraphTexture>(textures), access);
        }
        GraphBuffer dependency(const GraphBuffer buffer, const BufferAccess access) {
            return base.dependency(buffer, access);
        }
        GraphAccelerationStructure dependency(const GraphAccelerationStructure structure, const AccelerationStructureAccess access = AccelerationStructureAccess::Read) {
            return base.dependency(structure, access);
        }
        // dispatches in this pass are unordered unless requested, use when one dispatch reads what a previous one wrote (ie: mip chain)
        // has a performance overhead so only use if the commands in your pass need to be dependent on previous
        void executeCmdsInOrder() {
            this->base._passSource.executeInOrder = true;
        }
        // runs on the async compute queue, or on main when there is none
        void async() {
            this->base._passSource.async = true;
        }
        // keeps the pass when nothing the graph sees reads its output (queries, bindless writes, debug printf)
        void sideEffect() {
            this->base._passSource.sideEffect = true;
        }
    private:
        ComputePassBuilder(RenderGraph& rGraph, const char* pName) :
            base(rGraph, pName, PassDesc::Type::Compute) {}
        BasePassBuilder base;
        friend class RenderGraph;
    };

    class RayTracingPassBuilder {
    public:
        RayTracingPassBuilder() = delete;

        GraphTexture dependency(const TextureDesc& texDesc, const TextureAccess access = TextureAccess::StorageReadWrite) {
            return base.dependency(texDesc, access);
        }
        // each texture becomes the version this pass names or produces
        void dependency(const std::span<GraphTexture> textures, const TextureAccess access = TextureAccess::StorageReadWrite) {
            for (GraphTexture& texture: textures)
                texture = base.dependency(texture, access);
        }
        // const textures only take reads and keep their versions
        template<std::ranges::contiguous_range Range> requires std::same_as<std::ranges::range_reference_t<Range>, const GraphTexture&>
        void dependency(Range&& textures, const TextureAccess access = TextureAccess::StorageRead) {
            base.readDependencies(std::span<const GraphTexture>(textures), access);
        }
        GraphBuffer dependency(const GraphBuffer buffer, const BufferAccess access) {
            return base.dependency(buffer, access);
        }
        GraphAccelerationStructure dependency(const GraphAccelerationStructure structure, const AccelerationStructureAccess access = AccelerationStructureAccess::Read) {
            return base.dependency(structure, access);
        }
        void executeCmdsInOrder() {
            this->base._passSource.executeInOrder = true;
        }
        // runs on the async compute queue, or on main when there is none
        void async() {
            this->base._passSource.async = true;
        }
        // keeps the pass when nothing the graph sees reads its output (queries, bindless writes, debug printf)
        void sideEffect() {
            this->base._passSource.sideEffect = true;
        }
    private:
        RayTracingPassBuilder(RenderGraph& rGraph, const char* pName) :
            base(rGraph, pName, PassDesc::Type::RayTracing) {}
        BasePassBuilder base;
        friend class RenderGraph;
    };

    class TransferBuilder {
    public:
        TransferBuilder() = delete;

        // return the version of dst this pass produces
        GraphTexture copy(TextureDesc src, TextureDesc dst);
        GraphTexture blit(TextureDesc src, TextureDesc dst);
        // copies all of src to the start of dst, returns the version of dst this pass produces
        GraphBuffer copy(GraphBuffer src, GraphBuffer dst);
        // one mip of src, tightly packed at the start of dst
        GraphBuffer readback(TextureDesc src, GraphBuffer dst);
        // fills every mip below the top one with blits, needs main so async asserts
        GraphTexture generateMipmaps(GraphTexture texture);
        // for the raw commands of an execute, transfer accesses only
        GraphTexture dependency(const TextureDesc& texDesc, TextureAccess access);
        GraphBuffer dependency(GraphBuffer buffer, BufferAccess access);
        // bytes are copied into the graph at setup, returns the version of buffer this pass produces
        GraphBuffer update(GraphBuffer buffer, std::span<const std::byte> bytes, size_t dstOffset = 0);
        GraphBuffer upload(GraphBuffer buffer, std::span<const std::byte> bytes, size_t dstOffset = 0);
        // one whole mip of dst, tightly packed like readback
        GraphTexture upload(TextureDesc dst, std::span<const std::byte> bytes);
        template<std::ranges::contiguous_range Range> requires std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>
        GraphTexture upload(TextureDesc dst, const Range& range) { return upload(dst, std::span<const std::byte>(std::as_bytes(std::span(range)))); }

        template<std::ranges::contiguous_range Range> requires std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>
        GraphBuffer update(GraphBuffer buffer, const Range& range, size_t dstOffset = 0) { return update(buffer, std::span<const std::byte>(std::as_bytes(std::span(range))), dstOffset); }
        template<typename T> requires (std::is_trivially_copyable_v<T> && !std::ranges::range<T>)
        GraphBuffer update(GraphBuffer buffer, const T& value, size_t dstOffset = 0) { return update(buffer, std::span<const std::byte>(std::as_bytes(std::span(&value, 1))), dstOffset); }
        template<std::ranges::contiguous_range Range> requires std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>
        GraphBuffer upload(GraphBuffer buffer, const Range& range, size_t dstOffset = 0) { return upload(buffer, std::span<const std::byte>(std::as_bytes(std::span(range))), dstOffset); }
        template<typename T> requires (std::is_trivially_copyable_v<T> && !std::ranges::range<T>)
        GraphBuffer upload(GraphBuffer buffer, const T& value, size_t dstOffset = 0) { return upload(buffer, std::span<const std::byte>(std::as_bytes(std::span(&value, 1))), dstOffset); }
        // inputs are every buffer the structure was created from, a TLAS also names the BLASes its instances point to
        GraphAccelerationStructure build(GraphAccelerationStructure structure, std::span<const GraphBuffer> inputs, std::span<const GraphAccelerationStructure> instanced = {});
        // like build, keeps the topology and needs VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
        GraphAccelerationStructure refit(GraphAccelerationStructure structure, std::span<const GraphBuffer> inputs, std::span<const GraphAccelerationStructure> instanced = {});
        // runs on the transfer queue, then async compute, then main, a blit needs main and asserts, a build skips the transfer queue
        void async() {
            this->base._passSource.async = true;
        }
    private:
        TransferBuilder(RenderGraph& rGraph, const char* pName) :
            base(rGraph, pName, PassDesc::Type::Transfer) {}
        GraphAccelerationStructure declareBuild(GraphAccelerationStructure structure, std::span<const GraphBuffer> inputs, std::span<const GraphAccelerationStructure> instanced, bool refit);
        BasePassBuilder base;
        friend class RenderGraph;
    };

    struct ScheduledPass {
        static constexpr uint32_t kCulled = UINT32_MAX;
        std::string name;
        PassDesc::Type type = PassDesc::Type::Graphics;
        QueueAffinity queue = QueueAffinity::Graphics;
        // per queue, a batch ends where another queue reads its work and a new one starts where it waits on another queue
        uint32_t batch = kCulled;
        // passes sharing a rendering scope share a group, it is also the record order
        uint32_t mergeGroup = kCulled;
        bool culled = false;
    };
    class RenderGraph {
    public:
        // really dont worry about these you wont need them, purely debugging
        struct Options {
            bool cull = true;
            bool merge = true;
            bool async = true;
            // an ALL_COMMANDS barrier between every pass
            bool fullBarriers = false;
            // schedule in declaration order and assert when that order breaks a dependency
            bool declaredOrder = false;
            bool operator==(const Options&) const = default;
        };
        RenderGraph();
        ~RenderGraph();
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) noexcept;
        RenderGraph& operator=(RenderGraph&&) noexcept;

        // setup runs now and fills Data, execute runs at record time and must not capture
        template<typename Data = NoPassData, typename Setup, typename Execute>
        void addGraphicsPass(const char* pName, Setup&& setup, Execute&& execute) {
            addPass<Data, GraphicsCmd, GraphicsPassBuilder>(pName, setup, execute);
        }
        template<typename Data = NoPassData, typename Setup, typename Execute>
        void addComputePass(const char* pName, Setup&& setup, Execute&& execute) {
            addPass<Data, ComputeCmd, ComputePassBuilder>(pName, setup, execute);
        }
        template<typename Data = NoPassData, typename Setup, typename Execute>
        void addRayTracingPass(const char* pName, Setup&& setup, Execute&& execute) {
            addPass<Data, RayTracingCmd, RayTracingPassBuilder>(pName, setup, execute);
        }
        template<typename Setup>
        void addTransferPass(const char* pName, Setup&& setup) {
            TransferBuilder builder{ *this, pName };
            setup(builder);
            addTransferDesc(std::move(builder.base._passSource));
        }
        // execute runs after the pass's operations
        template<typename Data = NoPassData, typename Setup, typename Execute>
        void addTransferPass(const char* pName, Setup&& setup, Execute&& execute) {
            addPass<Data, TransferCmd, TransferBuilder>(pName, setup, execute);
        }
        // compile is where ALL of the overhead should be, all data stored during setup is now translated into pieces that are easier to direct with
        void compile(CTX& rCtx);

        // records, submits, and presents the frame, acquire and present only happen when a surviving pass uses backBuffer()
        // a valid timestamps pool gets a begin/end pair per merge group at firstQuery + 2 * mergeGroup, host reset after acquire
        SubmitHandle execute(CTX& rCtx, QueryPoolHandle timestamps = {}, uint32_t firstQuery = 0);

        void reset();

        // owned by the graph, created at compile with the usage its surviving passes need
        [[nodiscard]] GraphTexture createTexture(GraphTextureSpec spec);
        // non-owning, the caller keeps the texture alive for the graph's lifetime, importing the same texture twice returns the same id
        [[nodiscard]] GraphTexture importTexture(const Texture& texture);
        [[nodiscard]] GraphTexture importTexture(const Texture& texture, const ExternalAccess& external);
        // whichever swapchain image is current when the graph records
        [[nodiscard]] GraphTexture backBuffer();
        // non-owning like importTexture, every buffer a pass touches goes through here
        [[nodiscard]] GraphBuffer importBuffer(const Buffer& buffer);
        [[nodiscard]] GraphBuffer importBuffer(const Buffer& buffer, const ExternalAccess& external);
        // non-owning like importBuffer, its storage and scratch buffers become graph buffers
        [[nodiscard]] GraphAccelerationStructure importAccelerationStructure(const AccelerationStructure& structure);

        void setOptions(const Options& options) { _options = options; }
        const Options& getOptions() const { return _options; }

        // the last compile in record order, then its culled passes, kept across reset()
        [[nodiscard]] std::span<const ScheduledPass> schedule() const { return _scheduleView; }
        // graphviz of the last compile with passes, versions, planned barriers and queues, valid until reset()
        [[nodiscard]] std::string dump() const;

        [[nodiscard]] GraphExtent createExtent(Dimensions dimensions);
        // textures following this extent resize at the next execute
        void setExtent(GraphExtent extent, Dimensions dimensions);
    private:
        template<typename Data, typename Cmd>
        struct BoundPass final : PassExecute {
            Data data{};
            void (*fn)(const Data&, Cmd&) = nullptr;
        };
        template<typename Data, typename Cmd>
        static void InvokePass(const PassExecute& self, CommandBuffer& cmd) {
            const auto& bound = static_cast<const BoundPass<Data, Cmd>&>(self);
            Cmd typed{ cmd };
            bound.fn(bound.data, typed);
        }
        template<typename Data, typename Cmd, typename Builder, typename Setup, typename Execute>
        void addPass(const char* pName, Setup& setup, Execute& execute) {
            static_assert(std::is_convertible_v<Execute, void (*)(const Data&, Cmd&)>, "execute must not capture and must take (const Data&, Cmd&), put what it needs in Data from setup");
            auto& bound = _arena.create<BoundPass<Data, Cmd>>();
            bound.invoke = &InvokePass<Data, Cmd>;
            bound.fn = execute;
            Builder builder{ *this, pName };
            setup(builder, bound.data);
            builder.base._passSource.execute = &bound;
            addPassDesc(std::move(builder.base._passSource));
        }
        GraphTexture readVersion(const PassDesc& pass, GraphTexture texture) const;
        GraphTexture writeVersion(const PassDesc& pass, GraphTexture texture);
        GraphBuffer readVersion(const PassDesc& pass, GraphBuffer buffer) const;
        GraphBuffer writeVersion(const PassDesc& pass, GraphBuffer buffer);
        BufferHandle handleOf(GraphBuffer buffer) const;
        GraphBuffer storageOf(GraphAccelerationStructure structure) const;
        const std::vector<uint32_t>& producersOf(uint32_t id) const;
        std::optional<uint32_t> producerOf(GraphBuffer buffer) const;
        std::string_view nameOf(GraphTexture texture) const;
        uint32_t mipCountOf(GraphTexture texture) const;
        void addPassDesc(PassDesc&& desc);
#ifdef DEBUG
        void checkPushResources(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, std::string_view pipeline, std::span<const PushResourceField> fields, const void* data, uint32_t size, uint32_t offset) const;
        void observeDispatch(const CompiledPass& pass, const PassMember& member) const;
        void reportObservedUses(const CompiledPass& pass, const PassMember& member) const;
        void reportDepthBlockedMerges(size_t passIdx, bool drewFixedFormats) const;
        void reportDeclarations(const std::vector<uint32_t>& culled_order, const std::vector<PassUses>& passUses, std::unordered_set<std::string>& findings) const;
#endif
        void addTransferDesc(PassDesc&& desc);

        Dimensions extentOf(const CTX& rCtx, GraphExtent extent) const;
        Dimensions sizeOf(const CTX& rCtx, const GraphTextureSpec& spec) const;
        ResolvedTexture resolve(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, GraphTexture texture) const;
        void PerformBarrierTransitions(CommandBuffer& cmd, std::span<const CompiledImageBarrier> imageBarriers, std::span<const CompiledBufferBarrier> bufferBarriers);
        void realizeDeclared(CTX& rCtx, const std::vector<uint32_t>& culledOrder, const std::vector<bool>& confined);
        void RecordTransferOps(CommandBuffer& cmd, const CompiledPass& pass) const;
        void RecordReadback(CommandBuffer& cmd, const CompiledPass& pass, const ReadbackOp& op) const;
        void RecordImageUpload(CommandBuffer& cmd, const CompiledPass& pass, const ImageUploadOp& op) const;
        static void RecordGenerateMips(VkCommandBuffer cmd, const CompiledPass& pass, const detail::AllocatedTexture& tex);
        TextureShape shapeOf(const CTX& rCtx, GraphTexture texture) const;
        void planResourceAccess(const CTX& rCtx, const TextureDesc& texDesc, VkImageLayout desiredLayout, const StageAccess& dstMask, CompiledPass& outPass, const PassDesc& pass, bool keepsContents) const;
        static const ImmediateCommands& ImmOf(const CTX& rCtx, QueueAffinity queue);
        // stages a barrier source may name on a queue outside the graphics family, a source from another queue is already covered by its semaphore
        static StageAccess ClampToQueue(const CTX& rCtx, QueueAffinity queue, StageAccess mask);
        // true when it made a graph texture concurrent, the caller realizes and plans again
        bool planOwnership(const CTX& rCtx);
        void planExternal(const CTX& rCtx);
        void recordExternalEntry(CommandBuffer& cmd, ImmediateCommands& imm, const ExternalUse& use);
        void recordExternalExit(CommandBuffer& cmd, ImmediateCommands& imm, const ExternalUse& use) const;
        void recordRelease(CommandBuffer& cmd, OwnershipTransfer& transfer);
        void recordAcquire(CommandBuffer& cmd, const OwnershipTransfer& transfer);
        void planPassResources(const CTX& rCtx, const PassDesc& passDesc, CompiledPass& outPass) const;
        void planAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) const;
        void bindPass(CTX& rCtx, CompiledPass& pass) const;
        std::unordered_map<TextureHandle, TextureAccessCache> _resourceTrackers;
        std::unordered_map<BufferHandle, vkutil::BufferAccessTracker> _bufferTrackers;
        std::vector<GraphResource> _textures;
        std::vector<GraphBufferResource> _buffers;
        // producers[id][v - 1] is the pass that produced version v this frame
        std::vector<std::vector<uint32_t>> _textureProducers;
        std::vector<std::vector<uint32_t>> _bufferProducers;
        Options _options;
        // nullopt follows the swapchain
        std::vector<std::optional<Dimensions>> _extents{ std::nullopt };
        std::vector<PassDesc> _passDescriptions;
        std::vector<CompiledPass> _compiledPasses;

        bool _hasCompiled = false;
#ifdef DEBUG
        bool _hasReportedCompile = false;
#endif

        std::vector<ScheduledPass> _scheduleView;
        std::vector<OwnershipTransfer> _transfers;
        std::vector<uint32_t> _frameStartReleases;
        std::vector<uint32_t> _frameEndAcquires;
        std::vector<ExternalUse> _externals;

        TransientPool _transientPool;
        PassArena _arena;
#ifdef DEBUG
        std::unordered_set<std::string> _findings;
        // keys seen this record and the last one, a key in both happens every frame
        mutable std::unordered_set<std::string> _recordFindings;
        std::unordered_set<std::string> _lastRecordFindings;
        // a record finding prints once for the life of the graph, ping-pong resources would repeat it every other frame
        mutable std::unordered_set<std::string> _shownRecordFindings;
        mutable std::unordered_map<uint32_t, uint8_t> _observedUses;
        mutable std::vector<std::pair<BufferHandle, uint8_t>> _observedBufferUses;
        struct ObservedDispatches {
            // graph textures the pushes since the last dispatch name, the next dispatch uses them
            std::unordered_map<uint32_t, uint8_t> next;
            std::unordered_map<uint32_t, uint8_t> earlier;
            uint32_t count = 0;
            bool dispatchedSincePush = false;
            bool dependent = false;
            // a pointer or a texture outside the graph, the in-order barrier may order it
            bool untracked = false;
        };
        mutable ObservedDispatches _observedDispatches;
        // compiled index of each group whose merge with the next is blocked only by depth, record decides if depth can be added
        std::vector<uint32_t> _depthBlockedMerges;
#endif

        friend struct BasePassBuilder;
        friend class TransferBuilder;
        friend class CommandBuffer;
    };
}
