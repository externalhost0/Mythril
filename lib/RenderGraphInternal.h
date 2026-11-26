//
// Created by Hayden Rivas on 10/8/25.
//

#pragma once

#include "mythril/RenderGraphBuilder.h"
#include "mythril/RenderGraphDescriptions.h"
#include "mythril/SwapchainSpec.h"
#include "HelperMacros.h"
#include "SubresourceMap.h"
#include "vkutil.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <volk.h>


namespace mythril {

    struct GraphTextureAccess {
        static constexpr uint32_t index(GraphTexture texture) { return texture._index; }
        static constexpr uint32_t version(GraphTexture texture) { return texture._version; }
        static constexpr GraphTexture make(const uint32_t index, const uint32_t version = 0) { return GraphTexture{index, version}; }
    };
    struct GraphBufferAccess {
        static constexpr uint32_t index(GraphBuffer buffer) { return buffer._index; }
        static constexpr uint32_t version(GraphBuffer buffer) { return buffer._version; }
        static constexpr GraphBuffer make(const uint32_t index, const uint32_t version = 0) { return GraphBuffer{index, version}; }
    };
    struct GraphAccelerationStructureAccess {
        static constexpr uint32_t index(GraphAccelerationStructure structure) { return structure._storageSlot; }
        static constexpr uint32_t version(GraphAccelerationStructure structure) { return structure._version; }
        static constexpr GraphAccelerationStructure make(const uint32_t index, const uint32_t version = 0) { return GraphAccelerationStructure{index, version}; }
    };
    // ImGui::Image(GraphTexture) stores the graph id behind this tag, cmdDrawImGui swaps in the descriptor
    constexpr uint64_t kImGuiGraphTextureTag = 1ull << 63;
    struct GraphTextureHash {
        size_t operator()(const GraphTexture texture) const { return std::hash<uint32_t>{}(GraphTextureAccess::index(texture)); }
    };

    struct TextureUses {
        bool attachment = false;
        bool sampled = false;
        bool storage = false;
        bool transfer = false;

        void addAttachment() { attachment = true; }
        void add(const TextureAccess access) {
            switch (access) {
                case TextureAccess::Sampled:
                    sampled = true;
                    break;
                case TextureAccess::StorageRead:
                case TextureAccess::StorageWrite:
                case TextureAccess::StorageReadWrite:
                    storage = true;
                    break;
                case TextureAccess::TransferRead:
                case TextureAccess::TransferWrite:
                    transfer = true;
                    break;
            }
        }
    };
    struct Realization {
        Dimensions dimensions;
        TextureUsageBits usage{};
        StorageType storage = StorageType::Device;
        bool concurrent = false;
        bool operator==(const Realization&) const = default;
    };
    inline Dimensions ScaleExtent(const Dimensions extent, const float scale) {
        const auto scaled = [scale](const uint32_t v) { return std::max(1u, static_cast<uint32_t>(static_cast<float>(v) * scale)); };
        return {scaled(extent.width), scaled(extent.height), 1};
    }
    inline Realization InferRealization(const Dimensions dimensions, const TextureUses& uses, const bool confined) {
        TextureUsageBits usage{};
        if (uses.attachment)
            usage |= TextureUsageBits_Attachment;
        if (uses.sampled)
            usage |= TextureUsageBits_Sampled;
        if (uses.storage)
            usage |= TextureUsageBits_Storage;
        const bool memoryless = usage == TextureUsageBits_Attachment && !uses.transfer && confined;
        return {.dimensions = dimensions, .usage = usage, .storage = memoryless ? StorageType::Memoryless : StorageType::Device};
    }
    inline TextureSpec ToTextureSpec(const GraphTextureSpec& spec, const Realization& realization) {
        return {
            .dimension = realization.dimensions,
            .type = spec.type,
            .format = spec.format,
            .samples = spec.samples,
            .usage = realization.usage,
            .storage = realization.storage,
            .numMipLevels = spec.numMipLevels,
            .numLayers = spec.numLayers,
            .components = spec.components,
            .concurrent = realization.concurrent,
            .debugName = spec.debugName.c_str(),
        };
    }

	inline TransientKey KeyOf(const GraphTextureSpec& spec, const Realization& r) {
		return {
			.dimensions = r.dimensions,
			.type = spec.type,
			.format = spec.format,
			.samples = spec.samples,
			.numMips = spec.numMipLevels,
			.numLayers = spec.numLayers,
			.components = spec.components,
			.usage = r.usage,
			.storage = r.storage,
			.concurrent = r.concurrent
		};
	}

    struct DeclaredTexture {
        GraphTextureSpec spec;
        TextureHandle texture;
        Realization realized;
        // set once two families read it with no order between them, and kept so the pool never flips it back
        bool concurrent = false;
    };
    struct ImportedTexture {
        TextureHandle handle;
        std::string debugName;
        uint32_t numMips = 1;
    };
    struct SwapchainTexture {};
    struct GraphResource {
        std::variant<DeclaredTexture, ImportedTexture, SwapchainTexture> source;
        std::optional<ExternalAccess> external;
    };
    inline uint32_t IdOf(const GraphTexture texture) { return GraphTextureAccess::index(texture); }
    inline uint32_t VersionOf(const GraphTexture texture) { return GraphTextureAccess::version(texture); }
    inline const GraphResource& ResourceOf(const std::vector<GraphResource>& textures, const GraphTexture texture) {
        ASSERT_MSG(texture.valid() && IdOf(texture) < textures.size(), "GraphTexture does not belong to this RenderGraph.");
        return textures[IdOf(texture)];
    }
    struct GraphStructureSlot {
        AccelerationStructureHandle handle;
        uint32_t scratch = UINT32_MAX;
        bool topLevel = false;
        bool allowsRefit = false;
        std::vector<BufferHandle> inputs;
    };
    struct GraphBufferResource {
        BufferHandle handle;
        std::string debugName;
        std::optional<ExternalAccess> external;
        std::optional<GraphStructureSlot> structure;
    };
    struct ExternalUse {
        static constexpr uint32_t kUnused = UINT32_MAX;
        // invalid for a buffer
        GraphTexture texture;
        BufferHandle buffer;
        ExternalAccess access;
        uint32_t first = kUnused;
        uint32_t last = kUnused;
    };

    struct TextureShape {
        std::string_view debugName;
        VkFormat format;
        uint8_t sampleCount;
        Dimensions dimensions;
        uint32_t numMips;
        uint32_t numLayers;
    };

    struct AttachmentTarget {
        GraphTexture resource;
        SubresourceRange range;
        bool subresourceView = false;
    };

    struct AttachmentInfo {
        VkFormat imageFormat;
        VkImageView imageView;
        VkImageLayout imageLayout;
        VkImageView resolveImageView;
        VkImageLayout resolveImageLayout;

        VkAttachmentLoadOp loadOp;
        VkAttachmentStoreOp storeOp;

        VkClearValue clearValue;
        AttachmentTarget target;
        std::optional<AttachmentTarget> resolveTarget;
        // imageView, resolveImageView, texture and swapchainImageViews are filled by bind after merging
        TextureHandle texture;

        // will need to handle the special case of using a swapchain image as an attachment
        bool isSwapchainImage = false;
        VkImageView swapchainImageViews[kMAX_SWAPCHAIN_IMAGES] = {};

        VkRenderingAttachmentInfo getAsVkRenderingAttachmentInfo() const {
            const bool isResolving = resolveImageView != VK_NULL_HANDLE;
            return {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = imageView,
                .imageLayout = imageLayout,
                .resolveMode = isResolving ? (vkutil::IsIntegerFormat(imageFormat) || vkutil::IsFormatDepthOrStencil(imageFormat) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT) : VK_RESOLVE_MODE_NONE,
                .resolveImageView = resolveImageView,
                .resolveImageLayout = resolveImageLayout,
                .loadOp = loadOp,
                .storeOp = storeOp,
                .clearValue = clearValue
            };
        }
    };

    // what the graph last recorded per subresource
    // only trusted while the texture's layout version still matches
    struct TextureAccessCache {
        SubresourceMap<StageAccess> masks;
        uint32_t layoutVersion = 0;
    };

    struct CompiledImageBarrier {
        GraphTexture resource;
        // filled by bind, unused for the swapchain which resolves per frame
        TextureHandle handle;
        SubresourceRange range{};
        VkImageLayout dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        StageAccess dstMask{};
        bool isSwapchain = false;
        bool keepsContents = true;
        bool discardContents = false;
    };
    struct CompiledBufferBarrier {
        BufferHandle handle;
        StageAccess dstMask{};
    };
    // barriers in one vkCmdPipelineBarrier2 do not chain, so a buffer gets one barrier per pass that covers every use
    inline void AddBufferBarrier(std::vector<CompiledBufferBarrier>& barriers, const BufferHandle handle, const StageAccess dstMask) {
        const auto it = std::ranges::find(barriers, handle, &CompiledBufferBarrier::handle);
        if (it == barriers.end()) {
            barriers.push_back({handle, dstMask});
            return;
        }
        it->dstMask.stage |= dstMask.stage;
        it->dstMask.access |= dstMask.access;
    }

    struct PassMember {
        std::string name;
        const PassExecute* execute = nullptr;
        std::vector<GraphTexture> sortedDeclaredTextures;
        std::vector<TextureDependencyDesc> textureDependencies;
        std::vector<BufferDependencyDesc> bufferDependencies;
    };

    struct CompiledPass {
        // transferred info from source
        std::string name;
        PassDesc::Type type;

        std::vector<CompiledImageBarrier> imageBarriers;
        std::vector<CompiledBufferBarrier> bufferBarriers;
        std::vector<CompiledImageBarrier> exitBarriers;
        // callbacks recorded in order inside this pass's barriers and rendering scope, empty for transfer passes
        std::vector<PassMember> members;
        std::vector<TransferOp> transferOps;
        std::optional<VkMemoryBarrier2> inOrderBarrier;

        // new info transformed from source on compile()
        std::vector<AttachmentInfo> colorAttachments;
        std::optional<AttachmentInfo> depthAttachment;
        VkRect2D renderArea;
        uint32_t layerCount = 1;
        uint32_t viewMask = 0;
    	QueueAffinity queue = QueueAffinity::Graphics;
        uint32_t batch = 0;
        std::array<uint32_t, kQueueCount> batchWaits{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        std::vector<uint32_t> releases;
        std::vector<uint32_t> acquires;
        std::vector<uint32_t> externalEntries;
        std::vector<uint32_t> externalExits;
        std::vector<uint32_t> descIndices;
    };

    // indexed by GraphTexture id
    inline std::vector<bool> ConfinedToOneScope(const std::span<const CompiledPass> groups, const size_t textureCount) {
        constexpr size_t kNoGroup = SIZE_MAX;
        std::vector<uint32_t> groupsUsing(textureCount, 0);
        std::vector<size_t> lastGroup(textureCount, kNoGroup);
        std::vector<bool> isAttachment(textureCount, false);
        std::vector<bool> disqualified(textureCount, false);
        for (size_t groupIdx = 0; groupIdx < groups.size(); ++groupIdx) {
            const CompiledPass& group = groups[groupIdx];
            for (const PassMember& member: group.members) {
                for (const GraphTexture texture: member.sortedDeclaredTextures) {
                    const uint32_t id = GraphTextureAccess::index(texture);
                    if (lastGroup[id] != groupIdx) {
                        lastGroup[id] = groupIdx;
                        ++groupsUsing[id];
                    }
                }
            }
            const auto visit = [&](const AttachmentInfo& attachment) {
                const uint32_t id = GraphTextureAccess::index(attachment.target.resource);
                isAttachment[id] = true;
                if (attachment.isSwapchainImage || attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || attachment.storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
                    disqualified[id] = true;
                if (attachment.resolveTarget.has_value())
                    disqualified[GraphTextureAccess::index(attachment.resolveTarget->resource)] = true;
            };
            for (const AttachmentInfo& color: group.colorAttachments)
                visit(color);
            if (group.depthAttachment.has_value())
                visit(*group.depthAttachment);
        }
        std::vector<bool> confined(textureCount, false);
        for (size_t id = 0; id < textureCount; ++id)
            confined[id] = groupsUsing[id] == 1 && isAttachment[id] && !disqualified[id];
        return confined;
    }

    struct ResourceUse {
        uint32_t id;
        uint32_t version;
        bool write;
    };
    struct PassUses {
        std::vector<ResourceUse> textures;
        std::vector<ResourceUse> buffers;
    };
    struct PassEdges {
        std::vector<std::unordered_set<uint32_t>> data;
        std::vector<std::unordered_set<uint32_t>> order;
    };
    inline bool isWrite(const TextureAccess access) {
        return access == TextureAccess::StorageWrite || access == TextureAccess::StorageReadWrite || access == TextureAccess::TransferWrite;
    }
    inline bool isWrite(const BufferAccess access) {
        return access == BufferAccess::ShaderReadWrite || access == BufferAccess::ShaderWrite || access == BufferAccess::TransferWrite || access == BufferAccess::AccelerationStructureBuildWrite ||
            access == BufferAccess::AccelerationStructureScratch;
    }
    inline constexpr const char* kDepthOnOneBlocker = "a depth attachment on only one of them";
    inline bool IsStructureAccess(const BufferAccess access) {
        return access == BufferAccess::AccelerationStructureRead || access == BufferAccess::AccelerationStructureBuildInput || access == BufferAccess::AccelerationStructureBuildRead ||
            access == BufferAccess::AccelerationStructureBuildWrite || access == BufferAccess::AccelerationStructureScratch;
    }
#ifdef DEBUG
    void CheckBuildHazards(const PassDesc& pass, const std::vector<GraphBufferResource>& buffers);
#endif
    // the least capable queue type a pass can run on, and what keeps it on the graphics queue
    struct QueueNeed {
        PassDesc::Type type;
        const char* graphicsReason = nullptr;
    };
    QueueNeed QueueNeedOf(const PassDesc& pass, const std::vector<GraphResource>& textures);
#ifdef DEBUG
    constexpr uint8_t kReadUse = 1;
    constexpr uint8_t kWriteUse = 2;
    constexpr uint8_t kStorageUse = 4;
    constexpr uint8_t kUnknownUse = 8;
    constexpr uint8_t kMayWrite = kWriteUse | kUnknownUse;
    enum class UseMismatch : uint8_t { None, StorageAsSampled, ReadDeclaredWritten, WriteDeclaredRead, WiderAccess };
    inline UseMismatch FindTextureUseMismatch(const std::span<const TextureDependencyDesc> declared, const GraphTexture texture, const uint8_t use) {
        bool sampled = false, storage = false, storageWrite = false, write = false, read = false, pureRead = false;
        for (const TextureDependencyDesc& dependency: declared) {
            if (GraphTextureAccess::index(dependency.texDesc.texture) != GraphTextureAccess::index(texture))
                continue;
            const TextureAccess access = dependency.access;
            sampled |= access == TextureAccess::Sampled;
            storage |= access == TextureAccess::StorageRead || access == TextureAccess::StorageWrite || access == TextureAccess::StorageReadWrite;
            storageWrite |= access == TextureAccess::StorageWrite;
            write |= isWrite(access);
            pureRead |= !isWrite(access);
            read |= !isWrite(access) || access == TextureAccess::StorageReadWrite;
        }
        if (sampled && !storage && (use & kStorageUse))
            return UseMismatch::StorageAsSampled;
        if (!write && (use & kWriteUse))
            return UseMismatch::ReadDeclaredWritten;
        if (storageWrite && !read && (use & kReadUse))
            return UseMismatch::WriteDeclaredRead;
        if (write && !pureRead && (use & kReadUse) && !(use & kMayWrite))
            return UseMismatch::WiderAccess;
        return UseMismatch::None;
    }
    inline UseMismatch FindBufferUseMismatch(const std::span<const BufferDependencyDesc> declared, const BufferHandle buffer, const uint8_t use) {
        bool shaderRead = false, shaderWrite = false, shaderReadWrite = false;
        for (const BufferDependencyDesc& dependency: declared) {
            if (dependency.handle != buffer)
                continue;
            shaderRead |= dependency.access == BufferAccess::ShaderRead;
            shaderWrite |= dependency.access == BufferAccess::ShaderWrite;
            shaderReadWrite |= dependency.access == BufferAccess::ShaderReadWrite;
        }
        if (shaderRead && !shaderWrite && !shaderReadWrite && (use & kWriteUse))
            return UseMismatch::ReadDeclaredWritten;
        if (shaderWrite && !shaderRead && !shaderReadWrite && (use & kReadUse))
            return UseMismatch::WriteDeclaredRead;
        if ((shaderWrite || shaderReadWrite) && !shaderRead && (use & kReadUse) && !(use & kMayWrite))
            return UseMismatch::WiderAccess;
        return UseMismatch::None;
    }
#endif
    inline PassUses UsesOf(const PassDesc& desc) {
        PassUses uses;
        const auto texture = [&](const GraphTexture tex, const bool write) { uses.textures.push_back({GraphTextureAccess::index(tex), GraphTextureAccess::version(tex), write}); };
        for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations)
            texture(dependency.texDesc.texture, isWrite(dependency.access));
        for (const AttachmentDesc& attachment: desc.attachmentOperations) {
            texture(attachment.texDesc.texture, true);
            if (attachment.resolveTexDesc.has_value())
                texture(attachment.resolveTexDesc->texture, true);
        }
        for (const BufferDependencyDesc& dependency: desc.bufferDependencyOperations)
            uses.buffers.push_back({GraphBufferAccess::index(dependency.buffer), GraphBufferAccess::version(dependency.buffer), isWrite(dependency.access)});
        return uses;
    }
    inline PassEdges BuildPassEdges(const std::span<const PassUses> passes, const size_t textureCount, const size_t bufferCount) {
        constexpr uint32_t kNone = UINT32_MAX;
        PassEdges edges{std::vector<std::unordered_set<uint32_t>>(passes.size()), std::vector<std::unordered_set<uint32_t>>(passes.size())};
        const auto build = [&](std::vector<ResourceUse> PassUses::*member, const size_t count) {
            // producers[id][v] made version v + 1
            std::vector<std::vector<uint32_t>> producers(count);
            for (uint32_t i = 0; i < passes.size(); ++i) {
                for (const ResourceUse& use: passes[i].*member) {
                    if (!use.write)
                        continue;
                    std::vector<uint32_t>& made = producers[use.id];
                    if (made.size() <= use.version)
                        made.resize(use.version + 1, kNone);
                    made[use.version] = i;
                }
            }
            for (uint32_t i = 0; i < passes.size(); ++i) {
                for (const ResourceUse& use: passes[i].*member) {
                    const std::vector<uint32_t>& made = producers[use.id];
                    if (use.version >= 1 && use.version - 1 < made.size()) {
                        if (const uint32_t producer = made[use.version - 1]; producer != kNone && producer != i) {
                            edges.data[i].insert(producer);
                            edges.order[i].insert(producer);
                        }
                    }
                    if (!use.write && use.version < made.size()) {
                        if (const uint32_t next = made[use.version]; next != kNone && next != i)
                            edges.order[next].insert(i);
                    }
                }
            }
        };
        build(&PassUses::textures, textureCount);
        build(&PassUses::buffers, bufferCount);
        return edges;
    }

    struct ScheduleNode {
        std::vector<uint32_t> predecessors;
        bool async = false;
        bool readsAsync = false;
        bool renders = false;
    };
    struct ScheduleResult {
        std::vector<std::vector<uint32_t>> groups;
        std::vector<uint32_t> blocked;
    };
    // list scheduling, tryJoin(group, candidate) merges candidate into the open group and returns true, or leaves both untouched
    template<typename TryJoin>
    ScheduleResult SchedulePasses(const std::span<const ScheduleNode> nodes, const bool declaredOrder, const bool merge, TryJoin&& tryJoin) {
        ScheduleResult result;
        std::vector<uint32_t> group;
        const auto close = [&] {
            if (!group.empty())
                result.groups.push_back(std::move(group));
            group.clear();
        };
        if (declaredOrder) {
            for (uint32_t i = 0; i < nodes.size(); ++i) {
                if (std::ranges::any_of(nodes[i].predecessors, [i](const uint32_t pred) { return pred > i; })) {
                    result.blocked.push_back(i);
                    return result;
                }
                if (!group.empty() && merge && tryJoin(std::span<const uint32_t>(group), i)) {
                    group.push_back(i);
                    continue;
                }
                close();
                group.push_back(i);
            }
            close();
            return result;
        }
        std::vector<uint32_t> waiting(nodes.size());
        std::vector<std::vector<uint32_t>> successors(nodes.size());
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            waiting[i] = static_cast<uint32_t>(nodes[i].predecessors.size());
            for (const uint32_t pred: nodes[i].predecessors)
                successors[pred].push_back(i);
        }
        std::vector<uint32_t> ready;
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            if (waiting[i] == 0)
                ready.push_back(i);
        }
        const auto take = [&](const size_t readyIdx) {
            const uint32_t node = ready[readyIdx];
            ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(readyIdx));
            for (const uint32_t next: successors[node]) {
                if (--waiting[next] == 0)
                    ready.insert(std::ranges::upper_bound(ready, next), next);
            }
            return node;
        };
        size_t scheduled = 0;
        while (scheduled < nodes.size()) {
            bool joined = false;
            if (!group.empty() && merge) {
                for (size_t r = 0; r < ready.size(); ++r) {
                    if (tryJoin(std::span<const uint32_t>(group), ready[r])) {
                        group.push_back(take(r));
                        joined = true;
                        break;
                    }
                }
            }
            if (joined) {
                ++scheduled;
                continue;
            }
            close();
            if (ready.empty())
                break;
            const auto key = [&](const uint32_t node) {
                const ScheduleNode& n = nodes[node];
                return std::tuple{!n.async, n.readsAsync, n.renders, node};
            };
            const auto best = std::ranges::min_element(ready, {}, key);
            group.push_back(take(static_cast<size_t>(best - ready.begin())));
            ++scheduled;
        }
        close();
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            if (waiting[i] > 0)
                result.blocked.push_back(i);
        }
        return result;
    }

    struct AsyncQueues {
        bool compute = false;
        bool transfer = false;
    };
    inline QueueAffinity QueueOf(const PassDesc::Type type, const bool async, const AsyncQueues available) {
        if (!async)
            return QueueAffinity::Graphics;
        if (type == PassDesc::Type::Transfer && available.transfer)
            return QueueAffinity::AsyncTransfer;
        return available.compute ? QueueAffinity::AsyncCompute : QueueAffinity::Graphics;
    }

    struct BatchSlot {
        static constexpr uint32_t kNone = UINT32_MAX;
        uint32_t batch = 0;
        std::array<uint32_t, kQueueCount> waits{kNone, kNone, kNone};
    };
    inline std::vector<BatchSlot> SplitBatches(const std::span<const QueueAffinity> queues, const std::span<const std::vector<uint32_t>> predecessors) {
        constexpr uint32_t kNone = BatchSlot::kNone;
        std::vector<BatchSlot> slots(queues.size());
        std::array<uint32_t, kQueueCount> current{};
        std::array<bool, kQueueCount> open{};
        std::array<std::array<uint32_t, kQueueCount>, kQueueCount> waited;
        for (auto& row: waited)
            row.fill(kNone);
        std::array<uint32_t, kQueueCount> first{};
        const auto close = [&](const size_t q) {
            ++current[q];
            open[q] = false;
            waited[q].fill(kNone);
        };
        for (uint32_t pass = 0; pass < queues.size(); ++pass) {
            const auto q = static_cast<size_t>(queues[pass]);
            std::array<uint32_t, kQueueCount> needs;
            needs.fill(kNone);
            for (const uint32_t pred: predecessors[pass]) {
                const auto r = static_cast<size_t>(queues[pred]);
                if (r != q && (needs[r] == kNone || slots[pred].batch > needs[r]))
                    needs[r] = slots[pred].batch;
            }
            const bool newWait = std::ranges::any_of(std::views::iota(size_t{0}, size_t{kQueueCount}), [&](const size_t r) {
                return needs[r] != kNone && (waited[q][r] == kNone || needs[r] > waited[q][r]);
            });
            if (open[q] && newWait)
                close(q);
            if (!open[q])
                first[q] = pass;
            for (size_t r = 0; r < kQueueCount; ++r) {
                if (needs[r] == kNone)
                    continue;
                waited[q][r] = waited[q][r] == kNone ? needs[r] : std::max(waited[q][r], needs[r]);
                slots[first[q]].waits[r] = waited[q][r];
                if (open[r] && current[r] == needs[r])
                    close(r);
            }
            slots[pass].batch = current[q];
            open[q] = true;
        }
        return slots;
    }

    struct OwnershipUse {
        uint32_t texture;
        SubresourceRange range;
        uint32_t barrier;
        bool keepsContents;
    };
    struct OwnershipPass {
        uint32_t family;
        std::vector<OwnershipUse> uses;
    };
    struct OwnershipTransfer {
        // releasePass is kFrameStart for an image the graphics family owns from before the frame, acquirePass is kFrameEnd for the return at the join
        static constexpr uint32_t kFrameStart = UINT32_MAX;
        static constexpr uint32_t kFrameEnd = UINT32_MAX;
        uint32_t texture;
        SubresourceRange range;
        uint32_t releasePass;
        uint32_t releaseBarrier;
        uint32_t acquirePass;
        uint32_t acquireBarrier;
        uint32_t srcFamily;
        uint32_t dstFamily;
        // filled when the release records, the acquire must repeat its layouts exactly
        std::vector<std::pair<SubresourceRange, VkImageLayout>> recordedLayouts;
    };
    struct OwnershipPlan {
        std::vector<OwnershipTransfer> transfers;
        std::vector<std::pair<uint32_t, uint32_t>> discards;
        std::vector<std::pair<uint32_t, uint32_t>> conflicts;
    };
    inline OwnershipPlan PlanOwnership(const std::span<const OwnershipPass> passes, const uint32_t graphicsFamily) {
        struct Owned {
            uint32_t texture;
            SubresourceRange range;
            uint32_t family;
            uint32_t lastPass;
            uint32_t lastBarrier;
        };
        constexpr uint32_t kNone = UINT32_MAX;
        OwnershipPlan plan;
        std::vector<Owned> owned;
        for (uint32_t pass = 0; pass < passes.size(); ++pass) {
            const uint32_t family = passes[pass].family;
            for (const OwnershipUse& use: passes[pass].uses) {
                const bool clashes = std::ranges::any_of(owned, [&](const Owned& o) {
                    return o.texture == use.texture && o.range != use.range && o.range.overlaps(use.range) && (o.family != family || family != graphicsFamily);
                });
                if (clashes) {
                    plan.conflicts.emplace_back(pass, use.barrier);
                    continue;
                }
                auto it = std::ranges::find_if(owned, [&](const Owned& o) { return o.texture == use.texture && o.range == use.range; });
                if (it == owned.end()) {
                    owned.push_back({use.texture, use.range, graphicsFamily, OwnershipTransfer::kFrameStart, kNone});
                    it = owned.end() - 1;
                }
                if (it->family != family) {
                    if (use.keepsContents)
                        plan.transfers.push_back({use.texture, use.range, it->lastPass, it->lastBarrier, pass, use.barrier, it->family, family});
                    else
                        plan.discards.emplace_back(pass, use.barrier);
                    it->family = family;
                }
                it->lastPass = pass;
                it->lastBarrier = use.barrier;
            }
        }
        for (const Owned& o: owned) {
            if (o.family != graphicsFamily)
                plan.transfers.push_back({o.texture, o.range, o.lastPass, o.lastBarrier, OwnershipTransfer::kFrameEnd, kNone, o.family, graphicsFamily});
        }
        return plan;
    }

    inline std::vector<bool> HasIndependentMainWork(const std::span<const QueueAffinity> queues, const std::span<const std::vector<uint32_t>> predecessors) {
        const size_t n = queues.size();
        std::vector<std::vector<bool>> before(n, std::vector<bool>(n, false));
        for (size_t pass = 0; pass < n; ++pass) {
            for (const uint32_t pred: predecessors[pass]) {
                before[pass][pred] = true;
                for (size_t i = 0; i < n; ++i)
                    before[pass][i] = before[pass][i] || before[pred][i];
            }
        }
        std::vector<bool> result(n, false);
        for (size_t pass = 0; pass < n; ++pass) {
            for (size_t other = 0; other < n && !result[pass]; ++other)
                result[pass] = other != pass && queues[other] == QueueAffinity::Graphics && !before[pass][other] && !before[other][pass];
        }
        return result;
    }

    std::vector<uint32_t> cull_unused_passes(const std::vector<uint32_t>& topo_sorted_order, const std::vector<std::unordered_set<uint32_t>>& predecessors, const std::vector<PassDesc>& pass_descs, const std::vector<GraphResource>& textures);

    // only pass types resolve here so LOG_PASS and ASSERT_PASS can't be handed a loose string
    inline std::string_view PassLogName(const PassDesc& pass) { return pass.name; }
    inline std::string_view PassLogName(const CompiledPass& pass) { return pass.name; }
    inline std::string_view PassLogName(const PassMember& pass) { return pass.name; }

} // namespace mythril
