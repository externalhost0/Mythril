#include "RenderGraphInternal.h"
#include "HelperMacros.h"
#include "Logger.h"
#include "VulkanObjects.h"
#include "mythril/CTX.h"
#include "mythril/RenderGraphBuilder.h"

#include <algorithm>
#include <ranges>

namespace mythril {
    using namespace detail;

#ifdef DEBUG
    void RenderGraph::reportDeclarations(const std::vector<uint32_t>& culled_order, const std::vector<PassUses>& passUses, std::unordered_set<std::string>& findings) const {
        const auto isNew = [&](const std::string& key) {
            return findings.insert(key).second && !_debug->findings.contains(key);
        };
        {
            constexpr Options kDefaults{};
            const auto option = [&](const char* name, const bool value, const bool fallback) {
                if (value != fallback && isNew(std::string("option ") + name))
                    LOG_SYSTEM_NOSOURCE(LogType::Warning, "RenderGraph option '{}' is {}, its default is {}. Options are for bisecting and A/B only.", name, value, fallback);
            };
            option("cull", _options.cull, kDefaults.cull);
            option("merge", _options.merge, kDefaults.merge);
            option("async", _options.async, kDefaults.async);
            option("fullBarriers", _options.fullBarriers, kDefaults.fullBarriers);
            option("declaredOrder", _options.declaredOrder, kDefaults.declaredOrder);
        }
        std::vector<bool> kept(_passDescriptions.size(), false);
        for (const uint32_t pass_idx: culled_order)
            kept[pass_idx] = true;
        std::unordered_set<uint64_t> consumedTextureVersions;
        const auto versionKey = [](const uint32_t id, const uint32_t version) {
            return (static_cast<uint64_t>(id) << 32) | version;
        };
        for (const uint32_t pass_idx: culled_order) {
            for (const ResourceUse& use: passUses[pass_idx].textures)
                consumedTextureVersions.insert(versionKey(use.id, use.version));
        }
        const auto isDeclared = [&](const GraphTexture texture) {
            return std::holds_alternative<DeclaredTexture>(ResourceOf(_textures, texture).source);
        };
        for (uint32_t pass_idx = 0; pass_idx < _passDescriptions.size(); ++pass_idx) {
            const PassDesc& desc = _passDescriptions[pass_idx];
            if (!kept[pass_idx]) {
                if (isNew("culled " + desc.name))
                    LOG_PASS(LogType::Suggestion, desc, "Was culled, no pass that reaches the back buffer, an import, a buffer or a side effect reads what it writes. Remove it, or mark it sideEffect() if it has effects the graph cannot see.");
                continue;
            }
            const bool declaresNothing = desc.attachmentOperations.empty() && desc.textureDependencyOperations.empty() && desc.bufferDependencyOperations.empty();
            if (desc.sideEffect && declaresNothing && isNew("unordered side effect " + desc.name))
                LOG_PASS(LogType::Warning, desc, "Is a side effect that declares no accesses, so nothing orders it against other passes. Declare the resources it touches.");
            for (const TextureDependencyDesc& dependency: desc.textureDependencyOperations) {
                const GraphTexture texture = dependency.texDesc.texture;
                if (!isWrite(dependency.access) && VersionOf(texture) == 0 && isDeclared(texture) && isNew("undefined read " + desc.name + " " + std::string(nameOf(texture))))
                    LOG_PASS(LogType::Warning, desc, "Reads graph texture '{}' before any pass writes it this frame, its contents are undefined. Read the version a writer returns.", nameOf(texture));
            }
            const std::vector<BufferDependencyDesc>& buffers = desc.bufferDependencyOperations;
            for (auto first = buffers.begin(); first != buffers.end(); ++first) {
                if (std::ranges::find(buffers.begin(), first, first->handle, &BufferDependencyDesc::handle) != first)
                    continue;
                std::vector<BufferAccess> accesses;
                for (const BufferDependencyDesc& dependency: buffers)
                    if (dependency.handle == first->handle)
                        accesses.push_back(dependency.access);
                const auto count = [&](const BufferAccess access) {
                    return std::ranges::count(accesses, access);
                };
                const auto shader = count(BufferAccess::ShaderRead) + count(BufferAccess::ShaderWrite) + count(BufferAccess::ShaderReadWrite);
                if (shader < 2 && std::ranges::none_of(accesses, [&](const BufferAccess access) {
                        return count(access) > 1;
                    }))
                    continue;
                const std::string_view name = std::ranges::find(_buffers, first->handle, &GraphBufferResource::handle)->debugName;
                if (!isNew("repeated buffer " + desc.name + " " + std::string(name)))
                    continue;
                const bool readWrite = count(BufferAccess::ShaderReadWrite) || (count(BufferAccess::ShaderRead) && count(BufferAccess::ShaderWrite));
                const char* merged = readWrite ? "ShaderReadWrite" : count(BufferAccess::ShaderRead) ? "ShaderRead"
                                                                                                     : "ShaderWrite";
                if (shader >= 2)
                    LOG_PASS(LogType::Suggestion, desc, "Declares buffer '{}' {} times with shader access. Declare it once as {}.", name, shader, merged);
                else
                    LOG_PASS(LogType::Suggestion, desc, "Declares buffer '{}' more than once with the same access. Remove the repeated declaration.", name);
            }
            for (const AttachmentDesc& attachment: desc.attachmentOperations) {
                const GraphTexture texture = attachment.texDesc.texture;
                if (!isDeclared(texture))
                    continue;
                if (attachment.loadOp == LoadOp::LOAD && VersionOf(texture) == 0 && isNew("undefined load " + desc.name + " " + std::string(nameOf(texture))))
                    LOG_PASS(LogType::Suggestion, desc, "Loads graph texture '{}' before any pass writes it this frame. Use LoadOp::CLEAR or LoadOp::DONT_CARE.", nameOf(texture));
                if (attachment.storeOp == StoreOp::STORE && !attachment.resolveTexDesc.has_value() && !consumedTextureVersions.contains(versionKey(IdOf(texture), VersionOf(texture) + 1)) &&
                    isNew("unread store " + desc.name + " " + std::string(nameOf(texture))))
                    LOG_PASS(LogType::Suggestion, desc, "Stores graph texture '{}' but no pass reads what it writes. Use StoreOp::DONT_CARE, which also allows memoryless.", nameOf(texture));
            }
            for (const TransferOp& op: desc.transferOps) {
                if (const auto* upload = std::get_if<UploadOp>(&op); upload && upload->bytes.size() <= 65536 && isNew("small upload " + desc.name))
                    LOG_PASS(LogType::Suggestion, desc, "Uploads {} bytes (<= 64KB), use TransferBuilder::update instead for a faster operation!", upload->bytes.size());
            }
        }
    }

    static uint8_t UseOf(const PushResourceField& field) {
        const uint8_t storage = field.kind == PushField::Kind::StorageTexture ? kStorageUse : 0;
        switch (field.access) {
            case PushField::Access::None:
                return storage;
            case PushField::Access::Unknown:
                return field.kind == PushField::Kind::SampledTexture ? kReadUse : storage | kUnknownUse;
            default:
                return storage | static_cast<uint8_t>(field.access);
        }
    }
    void RenderGraph::checkPushResources(const CTX& rCtx, const CompiledPass& pass, const PassMember& member, const std::string_view pipeline, const std::span<const PushResourceField> fields, const void* data, const uint32_t size, const uint32_t offset) const {
        const auto isNew = [&](std::string key) {
            return _debug->shownRecordFindings.insert(std::move(key)).second;
        };
        DebugState::ObservedDispatches& dispatches = _debug->observedDispatches;
        if (dispatches.dispatchedSincePush) {
            dispatches.next.clear();
            dispatches.dispatchedSincePush = false;
        }
        for (const PushResourceField& field: fields) {
            if (field.offset < offset || field.offset + sizeof(uint64_t) > offset + size || field.kind == PushField::Kind::Sampler)
                continue;
            uint64_t value = 0;
            std::memcpy(&value, static_cast<const std::byte*>(data) + (field.offset - offset), sizeof(value));
            if (field.kind == PushField::Kind::AccelerationStructure) {
                // shaders cannot write a structure, so a graph one never orders dispatches
                const auto graphStructure = std::ranges::find_if(_buffers, [&](const GraphBufferResource& buffer) {
                    const AllocatedAccelerationStructure* live = buffer.structure ? rCtx._accelerationStructurePool.get(buffer.structure->handle) : nullptr;
                    return live && live->_vkDeviceAddress == value;
                });
                if (graphStructure == _buffers.end()) {
                    dispatches.untracked = true;
                    continue;
                }
                const bool declared = std::ranges::any_of(member.bufferDependencies, [&](const BufferDependencyDesc& dependency) {
                    return dependency.handle == graphStructure->handle && dependency.access == BufferAccess::AccelerationStructureRead;
                });
                if (!declared && isNew("undeclared structure " + member.name + " " + graphStructure->debugName))
                    LOG_PASS(LogType::Warning, pass, "'{}' passes acceleration structure '{}' as '{}' of pipeline '{}' without declaring it, so nothing orders it after its build. Declare it with b.dependency.", member.name, graphStructure->debugName, field.varName, pipeline);
                continue;
            }
            if (field.kind == PushField::Kind::Pointer) {
                dispatches.untracked = true;
                for (const GraphBufferResource& buffer: _buffers) {
                    const AllocatedBuffer& allocated = rCtx.view(buffer.handle);
                    if (value < allocated._vkDeviceAddress || value >= allocated._vkDeviceAddress + allocated._bufferSize)
                        continue;
                    if (const auto it = std::ranges::find(_debug->observedBufferUses, buffer.handle, &std::pair<BufferHandle, uint8_t>::first); it != _debug->observedBufferUses.end())
                        it->second |= UseOf(field);
                    else
                        _debug->observedBufferUses.emplace_back(buffer.handle, UseOf(field));
                    if (std::ranges::find(member.bufferDependencies, buffer.handle, &BufferDependencyDesc::handle) == member.bufferDependencies.end() && isNew("undeclared buffer " + member.name + " " + buffer.debugName))
                        LOG_PASS(LogType::Warning, pass, "'{}' passes a pointer into buffer '{}' as '{}' of pipeline '{}' without declaring it, so nothing orders it. Declare the buffer.", member.name, buffer.debugName, field.varName, pipeline);
                    break;
                }
                continue;
            }
            const auto index = static_cast<uint32_t>(value);
            uint32_t id = 0;
            for (; id < _textures.size(); ++id) {
                TextureHandle handle;
                if (const auto* declared = std::get_if<DeclaredTexture>(&_textures[id].source))
                    handle = declared->texture;
                else if (const auto* imported = std::get_if<ImportedTexture>(&_textures[id].source))
                    handle = imported->handle;
                if (!handle.valid() || handle.index() != index)
                    continue;
                const GraphTexture texture = GraphTextureAccess::make(id);
                const uint8_t use = UseOf(field);
                _debug->observedUses[id] |= use;
                dispatches.next[id] |= use;
                if (!std::ranges::binary_search(member.sortedDeclaredTextures, id, {}, IdOf) && isNew("undeclared texture " + member.name + " " + std::string(nameOf(texture))))
                    LOG_PASS(LogType::Warning, pass, "'{}' passes graph texture '{}' as '{}' of pipeline '{}' without declaring it, so nothing orders or transitions it. Declare the texture.", member.name, nameOf(texture), field.varName, pipeline);
                break;
            }
            if (id == _textures.size())
                dispatches.untracked = true;
        }
    }
    void RenderGraph::observeDispatch(const CompiledPass& pass, const PassMember& member) const {
        const auto isNew = [&](std::string key) {
            return _debug->shownRecordFindings.insert(std::move(key)).second;
        };
        DebugState::ObservedDispatches& dispatches = _debug->observedDispatches;
        for (const auto& [id, use]: dispatches.next) {
            const auto it = dispatches.earlier.find(id);
            const uint8_t now = use & (kReadUse | kMayWrite);
            const uint8_t before = it == dispatches.earlier.end() ? 0 : it->second & (kReadUse | kMayWrite);
            if (!now || !before || ((now | before) & kMayWrite) == 0)
                continue;
            dispatches.dependent = true;
            const std::string_view name = nameOf(GraphTextureAccess::make(id));
            if (!pass.inOrderBarrier && isNew("unordered dispatch " + member.name))
                LOG_PASS(LogType::Warning, pass, "'{}' dispatches use '{}' after an earlier dispatch in the pass used it, and one of them may write it, but nothing orders the dispatches. Call executeCmdsInOrder().", member.name, name);
        }
        for (const auto& [id, use]: dispatches.next)
            dispatches.earlier[id] |= use;
        ++dispatches.count;
        dispatches.dispatchedSincePush = true;
    }
    void RenderGraph::reportDepthBlockedMerges(const size_t passIdx) const {
        for (const uint32_t g: _debug->depthBlockedMerges) {
            const CompiledPass& group = _compiledPasses[g];
            const CompiledPass& next = _compiledPasses[g + 1];
            const CompiledPass& depthless = group.depthAttachment.has_value() ? next : group;
            // a pipeline with fixed formats cannot draw in a scope with a depth attachment, so the merge is out of reach
            if (&depthless != &_compiledPasses[passIdx] || _debug->drewFixedFormats)
                continue;
            const std::string key = "merge blocked " + group.name + " " + next.name;
            if (_debug->shownRecordFindings.insert(key).second)
                LOG_PASS(LogType::Suggestion, next, "Runs right after '{}' and shares an attachment with it, but cannot share its rendering scope because of {}. Give '{}' the same depth attachment with LoadOp::LOAD to merge them.", group.name, kDepthOnOneBlocker, depthless.name);
        }
    }
    void RenderGraph::reportObservedUses(const CompiledPass& pass, const PassMember& member) const {
        const auto isNew = [&](std::string key) {
            return _debug->shownRecordFindings.insert(std::move(key)).second;
        };
        for (const TextureDependencyDesc& dependency: member.textureDependencies) {
            const auto it = _debug->observedUses.find(IdOf(dependency.texDesc.texture));
            if (it == _debug->observedUses.end())
                continue;
            const std::string_view name = nameOf(dependency.texDesc.texture);
            const std::string key = member.name + " " + std::string(name);
            const uint8_t use = it->second;
            switch (FindTextureUseMismatch(member.textureDependencies, dependency.texDesc.texture, use)) {
                case UseMismatch::StorageAsSampled:
                    if (isNew("storage as sampled " + key))
                        LOG_PASS(LogType::Warning, pass, "'{}' declares '{}' Sampled, but its pipelines use it through a storage handle. Its barriers and layout only cover a sampled read, so a write races. Declare StorageRead, StorageWrite or StorageReadWrite.", member.name, name);
                    break;
                case UseMismatch::ReadDeclaredWritten:
                    if (isNew("read declared written " + key))
                        LOG_PASS(LogType::Warning, pass, "'{}' declares '{}' as a read, but its shaders write it, so nothing orders the write. Declare StorageWrite or StorageReadWrite.", member.name, name);
                    break;
                case UseMismatch::WriteDeclaredRead:
                    if (isNew("write declared read " + key))
                        LOG_PASS(LogType::Warning, pass, "'{}' declares '{}' StorageWrite, but its shaders also read it, so nothing orders the read after the previous write. Declare StorageReadWrite.", member.name, name);
                    break;
                case UseMismatch::WiderAccess:
                    if (isNew("wider access " + key))
                        LOG_PASS(LogType::Suggestion, pass, "'{}' declares '{}' as a write, but its shaders only read it. Declare {}.", member.name, name, use & kStorageUse ? "StorageRead" : "Sampled or StorageRead");
                    break;
                case UseMismatch::None:
                    break;
            }
        }
        for (const BufferDependencyDesc& dependency: member.bufferDependencies) {
            const auto it = std::ranges::find(_debug->observedBufferUses, dependency.handle, &std::pair<BufferHandle, uint8_t>::first);
            if (it == _debug->observedBufferUses.end())
                continue;
            const std::string_view name = std::ranges::find(_buffers, dependency.handle, &GraphBufferResource::handle)->debugName;
            const std::string key = member.name + " " + std::string(name);
            switch (FindBufferUseMismatch(member.bufferDependencies, dependency.handle, it->second)) {
                case UseMismatch::ReadDeclaredWritten:
                    if (isNew("read declared written " + key))
                        LOG_PASS(LogType::Warning, pass, "'{}' declares buffer '{}' ShaderRead, but its shaders write it through a pointer, so nothing orders the write. Declare ShaderWrite or ShaderReadWrite.", member.name, name);
                    break;
                case UseMismatch::WriteDeclaredRead:
                    if (isNew("write declared read " + key))
                        LOG_PASS(LogType::Warning, pass, "'{}' declares buffer '{}' ShaderWrite, but its shaders also read it, so nothing orders the read after the previous write. Declare ShaderReadWrite.", member.name, name);
                    break;
                case UseMismatch::WiderAccess:
                    if (isNew("wider access " + key))
                        LOG_PASS(LogType::Suggestion, pass, "'{}' declares buffer '{}' as a write, but its shaders only read it. Declare ShaderRead.", member.name, name);
                    break;
                case UseMismatch::StorageAsSampled:
                case UseMismatch::None:
                    break;
            }
        }
        const DebugState::ObservedDispatches& dispatches = _debug->observedDispatches;
        if (pass.inOrderBarrier && dispatches.count > 0 && !dispatches.dependent && !dispatches.untracked && isNew("needless in order " + member.name))
            LOG_PASS(LogType::Suggestion, pass, "'{}' calls executeCmdsInOrder(), but no dispatch uses a texture that an earlier dispatch may have written, or writes one an earlier dispatch used. Remove executeCmdsInOrder() to skip the barrier between dispatches.", member.name);
        _debug->observedUses.clear();
        _debug->observedBufferUses.clear();
        _debug->observedDispatches = {};
    }
#endif
} // namespace mythril
