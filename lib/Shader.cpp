//
// Created by Hayden Rivas on 11/1/25.
//

#include "ShaderReflection.h"
#include "SpirvLayout.h"
#include "HelperMacros.h"
#include "Logger.h"
#include "Pipelines.h"
#include "vkutil.h"
#include "mythril/CPUProfiler.h"
#include "mythril/CTX.h"
#include <vulkan/vk_enum_string_helper.h>
#include <fstream>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_set>

#include "mythril/Objects.h"

namespace mythril {
    using namespace detail;

    const char* ScalarKindToString(ScalarKind scalarKind) {
        switch (scalarKind) {
            case ScalarKind::None:
                return "None";
            case ScalarKind::Int:
                return "Int";
            case ScalarKind::UInt:
                return "UInt";
            case ScalarKind::Float:
                return "Float";
            case ScalarKind::Double:
                return "Double";
            case ScalarKind::Bool:
                return "Bool";
        }
        UNREACHABLE("Unsupported ScalarKind value.");
    }

    ReflectionResult ReflectSPIRV(const SpirvLayout& layout) {
        ReflectionResult result = {};
        VkShaderStageFlags vkShaderStageFlags = 0;
        for (const SpirvLayout::EntryPoint& entryPoint: layout.entryPoints) {
            const VkShaderStageFlagBits stage = ToShaderStage(entryPoint.model);
            result.entryPoints.push_back({ entryPoint.name, stage });
            vkShaderStageFlags |= stage;
        }

        for (const SpirvLayout::SpecConstant& constant: layout.specIds) {
            const auto type = layout.specConstantTypes.find(constant.id);
            if (type == layout.specConstantTypes.end())
                continue;
            SpecializationConstant info = {};
            info.varName = layout.name(constant.id);
            info.scalarKind = layout.scalarKind(type->second);
            info.id = constant.specId;
            info.size = layout.size(type->second);

            result.specializationInfo.specializationConstants.emplace_back(info);
            result.specializationInfo.nameToID.insert({ info.varName, info.id });
        }

        result.pipelineLayoutSignature.usesBindless = std::ranges::any_of(layout.resourceVariables, [&](const SpirvLayout::Variable& variable) {
            return layout.descriptorSets.contains(variable.id) && layout.bindings.contains(variable.id);
        });

        for (const SpirvLayout::Variable& variable: layout.pushVariables) {
            const uint32_t block = layout.pointee(variable.pointerType);
            uint32_t offset = std::numeric_limits<uint32_t>::max();
            for (uint32_t m = 0; m < layout.type(block).args.size(); ++m)
                offset = std::min(offset, layout.offset(block, m).value_or(0));
            result.pipelineLayoutSignature.pushes.push_back({
                .stageFlags = vkShaderStageFlags,
                .offset = offset,
                .size = layout.size(block) - offset,
            });
        }

        return result;
    }

    AllocatedShader CTX::buildShaderImpl(const SpirvLayout& layout, const char* debugName) const {
        AllocatedShader obj{};
        snprintf(obj._debugName, sizeof(obj._debugName), "%s", debugName);

        ReflectionResult reflection_result = ReflectSPIRV(layout);
        obj._pipelineSignature = reflection_result.pipelineLayoutSignature;
        for (SpirvPushBlock& block: DescribeSpirvPushBlocks(layout))
            obj._pipelineSignature.pushBlocks.push_back(std::move(block.layout));
#ifdef DEBUG
        obj._pipelineSignature.pushResources = AnalyzePushResources(layout, obj._pipelineSignature.pushBlocks);
#endif
        // specialization constants need to be stored until they are used in resolving a pipeline
        obj._specializationInfo = std::move(reflection_result.specializationInfo);
#ifdef DEBUG
        obj._entryPoints = std::move(reflection_result.entryPoints);
#endif

        // _vkShaderModule
        VkShaderModuleCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
        };
        create_info.flags = 0;
        create_info.pCode = layout.words().data();
        create_info.codeSize = layout.words().size_bytes();
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(_vkDevice, &create_info, nullptr, &shaderModule));
        vkutil::SetObjectDebugName(_vkDevice, VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(shaderModule), obj.getDebugName().data());
        obj._vkShaderModule = shaderModule;
        return obj;
    }

    // the shader rules are checked in debug builds only
    static std::optional<SpirvLayout> LoadSpirv(const std::filesystem::path& path, std::string& error) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            error = "It cannot be opened.";
            return std::nullopt;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0 || size % sizeof(uint32_t) != 0) {
            error = fmt::format("Its size of {} bytes is not a whole number of SPIR-V words.", static_cast<long long>(size));
            return std::nullopt;
        }
        std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(words.data()), size)) {
            error = "It cannot be read.";
            return std::nullopt;
        }
        SpirvLayout layout(std::move(words));
#ifdef DEBUG
        if (error = CheckSPIRVRules(layout); !error.empty())
            return std::nullopt;
#endif
        return layout;
    }

    Shader CTX::createShader(const ShaderSpec& spec) {
        MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
        std::string error;
        const std::optional<SpirvLayout> layout = LoadSpirv(spec.path, error);
        if (!layout)
            FATAL("Shader '{}' cannot load '{}'. {}", spec.debugName, spec.path.string(), error);
        ShaderHandle handle = _shaderPool.create(buildShaderImpl(*layout, spec.debugName));
        return { this, handle };
    }

#ifdef DEBUG
    static void ForEachStage(const GraphicsPipelineSpec& spec, const auto& fn) {
        if (spec.vertexShader.valid())
            fn(spec.vertexShader.handle, spec.vertexShader.entryPoint ? spec.vertexShader.entryPoint : kVertexEntryPoint, VK_SHADER_STAGE_VERTEX_BIT);
        if (spec.fragmentShader.valid())
            fn(spec.fragmentShader.handle, spec.fragmentShader.entryPoint ? spec.fragmentShader.entryPoint : kFragmentEntryPoint, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    static void ForEachStage(const ComputePipelineSpec& spec, const auto& fn) { fn(spec.shader, kComputeEntryPoint, VK_SHADER_STAGE_COMPUTE_BIT); }
    static void ForEachStage(const RayTracingPipelineSpec& spec, const auto& fn) {
        for (const auto& [stages, bit]: { std::pair{ &spec.rayGenShaders, VK_SHADER_STAGE_RAYGEN_BIT_KHR }, std::pair{ &spec.missShaders, VK_SHADER_STAGE_MISS_BIT_KHR },
                 std::pair{ &spec.closestHitShaders, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR }, std::pair{ &spec.callableShader, VK_SHADER_STAGE_CALLABLE_BIT_KHR } })
            for (const ShaderStage& stage: *stages)
                fn(stage.handle, stage.entryPoint, bit);
    }

    template<typename Pipeline>
    std::string CTX::findBuildProblemImpl(const Pipeline& pipeline) const {
        const auto& spec = pipeline._spec;
        const auto stageProblem = [&](const AllocatedShader& shader, const char* entryPoint, const VkShaderStageFlagBits stage) -> std::string {
            if (!entryPoint)
                return "its spec names no entry point";
            if (std::ranges::none_of(shader._entryPoints, [&](const ShaderEntryPoint& e) {
                    return e.stage == stage && e.name == entryPoint;
                }))
                return fmt::format("its spec names {} entry point '{}' which its shader does not have", string_VkShaderStageFlagBits(stage), entryPoint);
            const SpecializationInfo& info = shader._specializationInfo;
            if (info.nameToID.empty())
                return {};
            std::vector<uint32_t> setIds;
            for (const SpecializationConstantEntry& entry: spec.specConstants) {
                if (!entry.size)
                    break;
                const std::string* name = std::get_if<std::string>(&entry.identifier);
                if (name && !info.nameToID.contains(*name))
                    return fmt::format("its spec names specialization constant '{}' which its shader does not have", *name);
                const uint32_t id = name ? info.nameToID.at(*name) : std::get<int>(entry.identifier);
                setIds.push_back(id);
                // Vulkan ignores an id the shader does not have
                const auto constant = std::ranges::find(info.specializationConstants, id, &SpecializationConstant::id);
                if (constant != info.specializationConstants.end() && entry.size != constant->size)
                    return fmt::format("its specialization constant '{}' (id {}) is {} bytes but its shader reads a {}-byte {}", constant->varName, id, entry.size, constant->size, ScalarKindToString(constant->scalarKind));
            }
            for (const SpecializationConstant& constant: info.specializationConstants)
                if (std::ranges::find(setIds, constant.id) == setIds.end())
                    LOG_SYSTEM(LogType::Warning, "Shader '{}' reads specialization constant '{}' (id {}) that pipeline '{}' does not set, so it uses the shader's default value.", shader.getDebugName(), constant.varName, constant.id, pipeline.getDebugName());
            return {};
        };
        std::string problem;
        ForEachStage(spec, [&](const ShaderHandle handle, const char* entryPoint, const VkShaderStageFlagBits stage) {
            if (const AllocatedShader* shader = _shaderPool.get(handle); shader && problem.empty())
                problem = stageProblem(*shader, entryPoint, stage);
        });
        return problem;
    }
    template std::string CTX::findBuildProblemImpl(const AllocatedGraphicsPipeline&) const;
    template std::string CTX::findBuildProblemImpl(const AllocatedComputePipeline&) const;
    template std::string CTX::findBuildProblemImpl(const AllocatedRayTracingPipeline&) const;

    bool CTX::reloadShaderEXT(const Shader& shader, const std::filesystem::path& path) {
        MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_CREATE);
        AllocatedShader* current = _shaderPool.get(shader.handle());
        if (!current) {
            LOG_SYSTEM(LogType::Error, "'reloadShaderEXT' called on a null or destroyed shader.");
            return false;
        }
        if (_currentCommandBuffer._ctx) {
            LOG_SYSTEM(LogType::Error, "Cannot reload shader '{}' while a CommandBuffer is open, its recorded commands use the old pipelines.", current->getDebugName());
            return false;
        }
        std::string error;
        const std::optional<SpirvLayout> layout = LoadSpirv(path, error);
        if (!layout) {
            LOG_SYSTEM(LogType::Error, "Cannot reload shader '{}' from '{}', the old code stays. {}", current->getDebugName(), path.string(), error);
            return false;
        }

        AllocatedShader fresh = buildShaderImpl(*layout, current->_debugName);
        const auto pushHashes = [](const AllocatedShader& s) {
            std::vector<uint64_t> hashes;
            hashes.reserve(s._pipelineSignature.pushBlocks.size());
            for (const PushBlockLayout& block: s._pipelineSignature.pushBlocks)
                hashes.push_back(block.hash);
            std::ranges::sort(hashes);
            return hashes;
        };
        if (pushHashes(fresh) != pushHashes(*current)) {
            vkDestroyShaderModule(_vkDevice, fresh._vkShaderModule, nullptr);
            LOG_SYSTEM(LogType::Error, "Cannot reload shader '{}' from '{}', the old code stays. Its push layout changed, regenerate the headers and rebuild the C++.", current->getDebugName(), path.string());
            return false;
        }

        // the pipeline checks read the shader from the pool, so swap the new code in and back out if a pipeline cannot use it
        std::swap(*current, fresh);

        std::vector<SharedPipelineInfo*> users;
        std::string problem;
        forEachPipelinePoolImpl([&](auto& pool) {
            for (auto& obj: pool._objects) {
                bool uses = false;
                ForEachStage(obj._obj._spec, [&](const ShaderHandle stage, const char*, VkShaderStageFlagBits) {
                    uses |= stage == shader.handle();
                });
                if (!uses)
                    continue;
                if (std::string found = findBuildProblemImpl(obj._obj); !found.empty() && problem.empty())
                    problem = fmt::format("pipeline '{}' cannot use it, {}", obj._obj.getDebugName(), found);
                users.push_back(&obj._obj._shared);
            }
        });
        if (!problem.empty()) {
            std::swap(*current, fresh);
            vkDestroyShaderModule(_vkDevice, fresh._vkShaderModule, nullptr);
            LOG_SYSTEM(LogType::Error, "Cannot reload shader '{}' from '{}', the old code stays. The {}.", current->getDebugName(), path.string(), problem);
            return false;
        }
        deferTask(std::packaged_task<void()>([device = _vkDevice, module = fresh._vkShaderModule]() {
            vkDestroyShaderModule(device, module, nullptr);
        }));
        for (SharedPipelineInfo* shared: users)
            invalidatePipelineImpl(*shared);
        return true;
    }
#else
    bool CTX::reloadShaderEXT(const Shader&, const std::filesystem::path&) { return false; }
#endif

#ifdef DEBUG
    namespace {
        using Access = PushField::Access;
        constexpr uint32_t kNoClass = UINT32_MAX;

        // byte ranges of the push block a value comes from
        constexpr uint32_t kAnyIndex = UINT32_MAX;
        struct ByteRange {
            uint32_t lo;
            uint32_t hi;
            uint32_t type;
            std::vector<uint32_t> path;
            bool operator==(const ByteRange&) const = default;
        };
        using Tag = std::vector<ByteRange>;

        // a pointer or a uint2 that may be a handle, one per array element, array elements take the name of the field that holds them
        struct Leaf {
            PushField* node;
            std::string name;
            uint32_t offset;
        };
        void CollectLeaves(PushField& field, const uint32_t shift, const std::string_view name, std::vector<Leaf>& out) {
            switch (field.kind) {
                case PushField::Kind::Struct:
                    for (PushField& member: field.fields)
                        CollectLeaves(member, shift, member.name, out);
                    return;
                case PushField::Kind::Array:
                    for (uint32_t element = 0; element < field.count; ++element)
                        CollectLeaves(field.fields.front(), shift + element * field.stride, name, out);
                    return;
                case PushField::Kind::Pointer:
                    out.push_back({ &field, std::string(name), field.offset + shift });
                    return;
                case PushField::Kind::Vector:
                    if (field.columns == 2 && field.scalar == ScalarKind::UInt && field.bits == 32)
                        out.push_back({ &field, std::string(name), field.offset + shift });
                    return;
                default:
                    return;
            }
        }

        // forward walk from the push block to the image, pointer and atomic ops that use it
        class PushAccessWalk {
        public:
            PushAccessWalk(const SpirvLayout& layout, const std::span<const Leaf> leaves) :
                _layout(layout),
                _bound(layout.bound) {
                for (const Leaf& leaf: leaves)
                    _offsets.push_back(leaf.offset);
                _access.assign(leaves.size(), Access::None);
                _kinds.assign(leaves.size(), std::nullopt);
                _resultType.assign(_bound, 0);
                _variableClass.assign(_bound, kNoClass);
                _root.assign(_bound, 0);
                _tags.assign(_bound, {});
                _contents.assign(_bound, {});
                _rootPaths.assign(_bound, {});
                if (!parse() || !run())
                    std::ranges::fill(_access, Access::Unknown);
            }
            // the heap kind a handle leaf indexed, none when it never reached a heap
            std::optional<PushField::Kind> kind(const size_t leaf) const { return _kinds[leaf]; }
            Access access(const size_t leaf) const { return _access[leaf]; }

        private:
            bool run() {
                for (const SpirvLayout::Variable& variable: _layout.pushVariables)
                    _tags[variable.id] = { { 0, _layout.size(_layout.pointee(variable.pointerType)), _layout.pointee(variable.pointerType), {} } };
                for (uint32_t pass = 0; pass < 64; ++pass) {
                    _changed = false;
                    for (const std::span<const uint32_t> inst: _body)
                        transfer(inst);
                    if (!_changed)
                        return true;
                }
                return false;
            }
            void classifyIndex(const uint32_t heap, const Tag& tag) {
                uint32_t element = _layout.pointee(_resultType[heap]);
                if (op(element) == SpvOpTypeRuntimeArray || op(element) == SpvOpTypeArray)
                    element = arg(element, 0);
                const auto heap_kind = _heapKinds.find(element);
                if (heap_kind == _heapKinds.end())
                    return;
                classifyAs(tag, heap_kind->second);
            }
            void classifyAs(const Tag& tag, const PushField::Kind kind) {
                for (const ByteRange& range: tag)
                    for (size_t i = 0; i < _offsets.size(); ++i)
                        if (_offsets[i] >= range.lo && _offsets[i] < range.hi)
                            _kinds[i] = kind;
            }

            bool parse() {
                uint32_t function = 0;
                for (const std::span<const uint32_t> w: _layout.instructions) {
                    const auto count = static_cast<uint32_t>(w.size());
                    const auto opcode = static_cast<SpvOp>(w[0] & 0xffff);
                    const auto id = [&](const size_t k) {
                        return k < w.size() && w[k] < _bound ? w[k] : 0u;
                    };
                    switch (opcode) {
                        case SpvOpExtInstImport:
                            if (std::string_view(reinterpret_cast<const char*>(&w[2])).starts_with("NonSemantic."))
                                _nonSemanticSets.insert(w[1]);
                            break;
                        case SpvOpTypeImage:
                            if (count >= 9)
                                _heapKinds[w[1]] = w[7] == 2 ? PushField::Kind::StorageTexture : PushField::Kind::SampledTexture;
                            break;
                        case SpvOpTypeSampledImage:
                            _heapKinds[w[1]] = PushField::Kind::SampledTexture;
                            break;
                        case SpvOpTypeSampler:
                            _heapKinds[w[1]] = PushField::Kind::Sampler;
                            break;
                        case SpvOpTypeAccelerationStructureKHR:
                            _heapKinds[w[1]] = PushField::Kind::AccelerationStructure;
                            break;
                        case SpvOpVariable:
                            if (!id(2))
                                return false;
                            _resultType[w[2]] = w[1];
                            _variableClass[w[2]] = w[3];
                            if (w[3] == SpvStorageClassFunction || w[3] == SpvStorageClassPrivate)
                                _root[w[2]] = w[2];
                            break;
                        case SpvOpFunction:
                            if (!id(2))
                                return false;
                            function = w[2];
                            break;
                        case SpvOpFunctionParameter:
                            if (!function || !id(2))
                                return false;
                            _resultType[w[2]] = w[1];
                            _functionParams[function].push_back(w[2]);
                            if (op(w[1]) == SpvOpTypePointer && (arg(w[1], 0) == SpvStorageClassFunction || arg(w[1], 0) == SpvStorageClassPrivate))
                                _root[w[2]] = w[2];
                            break;
                        case SpvOpReturnValue:
                            if (function && id(1))
                                _functionReturns[function].push_back(w[1]);
                            break;
                        case SpvOpFunctionEnd:
                            function = 0;
                            break;
                        default:
                            break;
                    }
                    if (function)
                        _body.push_back(w);
                }
                return true;
            }

            SpvOp op(const uint32_t type) const { return _layout.type(type).op; }
            uint32_t arg(const uint32_t type, const size_t i) const { return _layout.arg(type, i); }
            uint32_t storageClass(const uint32_t value) const {
                const uint32_t type = _resultType[value];
                return op(type) == SpvOpTypePointer ? arg(type, 0) : kNoClass;
            }
            bool narrowToElement(const ByteRange& range, const uint32_t index, Tag& out) const {
                const uint32_t a = arg(range.type, 0);
                const uint32_t b = arg(range.type, 1);
                const std::optional<uint32_t> known = index == kAnyIndex ? std::nullopt : std::optional(index);
                switch (op(range.type)) {
                    case SpvOpTypeStruct: {
                        const std::vector<uint32_t>& members = _layout.type(range.type).args;
                        const std::optional<uint32_t> offset = known && *known < members.size() ? _layout.offset(range.type, *known) : std::nullopt;
                        if (!offset)
                            return false;
                        out.push_back({ range.lo + *offset, range.lo + *offset + _layout.size(members[*known]), members[*known], {} });
                        return true;
                    }
                    case SpvOpTypeArray:
                    case SpvOpTypeVector: {
                        // scalar layout packs elements, so a missing ArrayStride means the element size
                        const uint32_t stride = op(range.type) == SpvOpTypeVector ? _layout.size(a) : _layout.stride(range.type).value_or(_layout.size(a));
                        const uint32_t length = op(range.type) == SpvOpTypeVector ? b : _layout.constant(b).value_or(0);
                        const uint32_t first = known.value_or(0);
                        const uint32_t last = known ? *known + 1 : length;
                        if (stride == 0 || last > length)
                            return false;
                        for (uint32_t element = first; element < last; ++element)
                            out.push_back({ range.lo + element * stride, range.lo + (element + 1) * stride, a, {} });
                        return true;
                    }
                    case SpvOpTypeMatrix:
                        out.push_back({ range.lo, range.hi, a, {} });
                        return true;
                    default:
                        return false;
                }
            }
            Tag extract(const Tag& tag, const std::span<const uint32_t> path) {
                Tag current = tag;
                Tag next;
                for (const uint32_t index: path) {
                    next.clear();
                    for (ByteRange& range: current) {
                        if (range.path.empty()) {
                            if (!narrowToElement(range, index, next)) {
                                effect(tag, Access::Unknown);
                                return {};
                            }
                        } else if (index == kAnyIndex || range.path[0] == kAnyIndex || range.path[0] == index) {
                            range.path.erase(range.path.begin());
                            next.push_back(std::move(range));
                        }
                    }
                    std::swap(current, next);
                }
                return current;
            }
            std::vector<uint32_t> resolve(const std::span<const uint32_t> indices) const {
                std::vector<uint32_t> out;
                for (const uint32_t index: indices)
                    out.push_back(_layout.constant(index).value_or(kAnyIndex));
                return out;
            }
            static Tag prefixed(const Tag& tag, const std::span<const uint32_t> path) {
                Tag out = tag;
                for (ByteRange& range: out)
                    range.path.insert(range.path.begin(), path.begin(), path.end());
                return out;
            }
            void merge(Tag& into, const Tag& tag) {
                for (const ByteRange& range: tag) {
                    if (std::ranges::find(into, range) == into.end()) {
                        into.push_back(range);
                        _changed = true;
                    }
                }
            }
            void add(const uint32_t value, const Tag& tag) { merge(_tags[value], tag); }
            void effect(const Tag& tag, const Access access) {
                for (const ByteRange& range: tag)
                    for (size_t i = 0; i < _offsets.size(); ++i)
                        if (_offsets[i] >= range.lo && _offsets[i] < range.hi)
                            _access[i] = MergeAccess(_access[i], access);
            }
            const Tag& tagOf(const uint32_t value) const {
                static const Tag kEmpty;
                return value < _bound ? _tags[value] : kEmpty;
            }
            void transfer(const std::span<const uint32_t> w) {
                const auto opcode = static_cast<SpvOp>(w[0] & 0xffff);
                const auto count = static_cast<uint32_t>(w.size());
                switch (opcode) {
                    case SpvOpAccessChain:
                    case SpvOpInBoundsAccessChain: {
                        _resultType[w[2]] = w[1];
                        const uint32_t base = w[3];
                        if (_root[base]) {
                            _root[w[2]] = _root[base];
                            _rootPaths[w[2]] = _rootPaths[base];
                            std::ranges::copy(resolve(w.subspan(4)), std::back_inserter(_rootPaths[w[2]]));
                        }
                        if (!tagOf(base).empty())
                            add(w[2], storageClass(base) == SpvStorageClassPushConstant ? extract(tagOf(base), resolve(w.subspan(4))) : tagOf(base));
                        for (uint32_t k = 4; k < count; ++k) {
                            if (tagOf(w[k]).empty())
                                continue;
                            if (_variableClass[base] == SpvStorageClassUniformConstant) {
                                add(w[2], tagOf(w[k]));
                                classifyIndex(base, tagOf(w[k]));
                            } else
                                effect(tagOf(w[k]), Access::Unknown);
                        }
                        return;
                    }
                    case SpvOpPtrAccessChain:
                    case SpvOpInBoundsPtrAccessChain:
                        _resultType[w[2]] = w[1];
                        if (_root[w[3]]) {
                            _root[w[2]] = _root[w[3]];
                            _rootPaths[w[2]] = _rootPaths[w[3]];
                            if (!_rootPaths[w[2]].empty())
                                _rootPaths[w[2]].back() = kAnyIndex;
                            std::ranges::copy(resolve(w.subspan(5)), std::back_inserter(_rootPaths[w[2]]));
                        }
                        add(w[2], tagOf(w[3]));
                        for (uint32_t k = 4; k < count; ++k)
                            effect(tagOf(w[k]), Access::Unknown);
                        return;
                    case SpvOpLoad: {
                        _resultType[w[2]] = w[1];
                        const uint32_t pointer = w[3];
                        if (_root[pointer])
                            add(w[2], extract(_contents[_root[pointer]], _rootPaths[pointer]));
                        if (tagOf(pointer).empty())
                            return;
                        switch (storageClass(pointer)) {
                            case SpvStorageClassPhysicalStorageBuffer:
                                effect(tagOf(pointer), Access::Read);
                                return;
                            case SpvStorageClassPushConstant:
                            case SpvStorageClassUniformConstant:
                                add(w[2], tagOf(pointer));
                                return;
                            default:
                                effect(tagOf(pointer), Access::Unknown);
                                return;
                        }
                    }
                    case SpvOpStore: {
                        const uint32_t pointer = w[1];
                        const Tag& object = tagOf(w[2]);
                        if (!tagOf(pointer).empty())
                            effect(tagOf(pointer), storageClass(pointer) == SpvStorageClassPhysicalStorageBuffer ? Access::Write : Access::Unknown);
                        if (object.empty())
                            return;
                        if (_root[pointer])
                            merge(_contents[_root[pointer]], prefixed(object, _rootPaths[pointer]));
                        else
                            effect(object, Access::Unknown);
                        return;
                    }
                    case SpvOpCompositeExtract:
                        _resultType[w[2]] = w[1];
                        if (!tagOf(w[3]).empty())
                            add(w[2], extract(tagOf(w[3]), w.subspan(4)));
                        return;
                    case SpvOpCompositeConstruct:
                        _resultType[w[2]] = w[1];
                        for (uint32_t k = 3; k < count; ++k)
                            add(w[2], prefixed(tagOf(w[k]), std::array{ k - 3 }));
                        return;
                    case SpvOpCopyObject:
                    case SpvOpCopyLogical:
                    case SpvOpBitcast:
                    case SpvOpConvertUToPtr:
                    case SpvOpConvertPtrToU:
                    case SpvOpImage:
                        _resultType[w[2]] = w[1];
                        if (_root[w[3]]) {
                            _root[w[2]] = _root[w[3]];
                            _rootPaths[w[2]] = _rootPaths[w[3]];
                        }
                        add(w[2], tagOf(w[3]));
                        return;
                    // Slang joins the two words of an acceleration structure handle into its 64-bit address
                    case SpvOpUConvert:
                    case SpvOpShiftLeftLogical:
                    case SpvOpBitwiseOr:
                        _resultType[w[2]] = w[1];
                        add(w[2], tagOf(w[3]));
                        if (opcode == SpvOpBitwiseOr)
                            add(w[2], tagOf(w[4]));
                        return;
                    case SpvOpConvertUToAccelerationStructureKHR:
                        _resultType[w[2]] = w[1];
                        add(w[2], tagOf(w[3]));
                        classifyAs(tagOf(w[3]), PushField::Kind::AccelerationStructure);
                        return;
                    case SpvOpRayQueryInitializeKHR:
                        effect(tagOf(w[2]), Access::Read);
                        for (uint32_t k = 3; k < count; ++k)
                            effect(tagOf(w[k]), Access::Unknown);
                        return;
                    case SpvOpTraceRayKHR:
                        effect(tagOf(w[1]), Access::Read);
                        for (uint32_t k = 2; k < count; ++k)
                            effect(tagOf(w[k]), Access::Unknown);
                        return;
                    case SpvOpPhi:
                        _resultType[w[2]] = w[1];
                        for (uint32_t k = 3; k < count; k += 2)
                            add(w[2], tagOf(w[k]));
                        return;
                    case SpvOpSelect:
                        _resultType[w[2]] = w[1];
                        effect(tagOf(w[3]), Access::Unknown);
                        add(w[2], tagOf(w[4]));
                        add(w[2], tagOf(w[5]));
                        return;
                    case SpvOpSampledImage:
                        _resultType[w[2]] = w[1];
                        add(w[2], tagOf(w[3]));
                        add(w[2], tagOf(w[4]));
                        return;
                    case SpvOpImageTexelPointer:
                        _resultType[w[2]] = w[1];
                        add(w[2], tagOf(w[3]));
                        return;
                    case SpvOpImageSampleImplicitLod:
                    case SpvOpImageSampleExplicitLod:
                    case SpvOpImageSampleDrefImplicitLod:
                    case SpvOpImageSampleDrefExplicitLod:
                    case SpvOpImageSampleProjImplicitLod:
                    case SpvOpImageSampleProjExplicitLod:
                    case SpvOpImageSampleProjDrefImplicitLod:
                    case SpvOpImageSampleProjDrefExplicitLod:
                    case SpvOpImageFetch:
                    case SpvOpImageGather:
                    case SpvOpImageDrefGather:
                    case SpvOpImageRead:
                    case SpvOpImageSparseSampleImplicitLod:
                    case SpvOpImageSparseSampleExplicitLod:
                    case SpvOpImageSparseSampleDrefImplicitLod:
                    case SpvOpImageSparseSampleDrefExplicitLod:
                    case SpvOpImageSparseFetch:
                    case SpvOpImageSparseGather:
                    case SpvOpImageSparseDrefGather:
                    case SpvOpImageSparseRead:
                        effect(tagOf(w[3]), Access::Read);
                        return;
                    case SpvOpImageWrite:
                        effect(tagOf(w[1]), Access::Write);
                        return;
                    case SpvOpImageQueryFormat:
                    case SpvOpImageQueryOrder:
                    case SpvOpImageQuerySizeLod:
                    case SpvOpImageQuerySize:
                    case SpvOpImageQueryLod:
                    case SpvOpImageQueryLevels:
                    case SpvOpImageQuerySamples:
                        return;
                    case SpvOpAtomicLoad:
                        effect(tagOf(w[3]), Access::Read);
                        return;
                    case SpvOpAtomicStore:
                        effect(tagOf(w[1]), Access::Write);
                        effect(tagOf(w[4]), Access::Unknown);
                        return;
                    case SpvOpAtomicExchange:
                    case SpvOpAtomicCompareExchange:
                    case SpvOpAtomicCompareExchangeWeak:
                    case SpvOpAtomicIIncrement:
                    case SpvOpAtomicIDecrement:
                    case SpvOpAtomicIAdd:
                    case SpvOpAtomicISub:
                    case SpvOpAtomicSMin:
                    case SpvOpAtomicUMin:
                    case SpvOpAtomicSMax:
                    case SpvOpAtomicUMax:
                    case SpvOpAtomicAnd:
                    case SpvOpAtomicOr:
                    case SpvOpAtomicXor:
                    case SpvOpAtomicFlagTestAndSet:
                    case SpvOpAtomicFMinEXT:
                    case SpvOpAtomicFMaxEXT:
                    case SpvOpAtomicFAddEXT:
                        effect(tagOf(w[3]), Access::ReadWrite);
                        for (uint32_t k = 4; k < count; ++k)
                            effect(tagOf(w[k]), Access::Unknown);
                        return;
                    case SpvOpAtomicFlagClear:
                        effect(tagOf(w[1]), Access::Write);
                        return;
                    case SpvOpFunctionCall: {
                        const auto params = _functionParams.find(w[3]);
                        const size_t paramCount = params == _functionParams.end() ? 0 : params->second.size();
                        if (paramCount != count - 4)
                            break;
                        _resultType[w[2]] = w[1];
                        for (uint32_t k = 4; k < count; ++k) {
                            const uint32_t param = params->second[k - 4];
                            add(param, tagOf(w[k]));
                            if (_root[param] == param && _root[w[k]]) {
                                merge(_contents[param], extract(_contents[_root[w[k]]], _rootPaths[w[k]]));
                                merge(_contents[_root[w[k]]], prefixed(_contents[param], _rootPaths[w[k]]));
                            } else if (_root[w[k]])
                                effect(_contents[_root[w[k]]], Access::Unknown);
                        }
                        if (const auto returns = _functionReturns.find(w[3]); returns != _functionReturns.end())
                            for (const uint32_t value: returns->second)
                                add(w[2], tagOf(value));
                        return;
                    }
                    case SpvOpFunctionParameter:
                    case SpvOpReturnValue:
                        return;
                    case SpvOpExtInst:
                        if (_nonSemanticSets.contains(w[3]))
                            return;
                        break;
                    default:
                        break;
                }
                // an op the walk does not model loses the trace of every field it touches
                for (uint32_t k = 1; k < count; ++k)
                    effect(tagOf(w[k]), Access::Unknown);
            }

            std::vector<uint32_t> _offsets;
            std::vector<Access> _access;
            std::vector<std::optional<PushField::Kind>> _kinds;
            std::unordered_map<uint32_t, PushField::Kind> _heapKinds;
            const SpirvLayout& _layout;
            uint32_t _bound = 0;
            std::unordered_set<uint32_t> _nonSemanticSets;
            std::vector<uint32_t> _resultType;
            std::vector<uint32_t> _variableClass;
            // the Function or Private variable a pointer points into
            std::vector<uint32_t> _root;
            std::vector<Tag> _tags;
            // what was stored into each Function or Private variable
            std::vector<Tag> _contents;
            std::vector<std::vector<uint32_t>> _rootPaths;
            std::vector<std::span<const uint32_t>> _body;
            std::unordered_map<uint32_t, std::vector<uint32_t>> _functionParams;
            std::unordered_map<uint32_t, std::vector<uint32_t>> _functionReturns;
            bool _changed = false;
        };
    } // namespace

    std::vector<PushResourceField> AnalyzePushResources(const SpirvLayout& layout, const std::span<PushBlockLayout> blocks) {
        std::vector<Leaf> leaves;
        for (PushBlockLayout& block: blocks)
            CollectLeaves(block.root, 0, block.root.name, leaves);
        if (leaves.empty())
            return {};
        const PushAccessWalk walk(layout, leaves);
        std::vector<std::optional<PushField::Kind>> kinds;
        for (size_t i = 0; i < leaves.size(); ++i) {
            kinds.push_back(leaves[i].node->kind == PushField::Kind::Pointer ? PushField::Kind::Pointer : walk.kind(i));
            if (kinds.back())
                leaves[i].node->access = Access::None;
        }
        std::vector<PushResourceField> fields;
        for (size_t i = 0; i < leaves.size(); ++i) {
            const std::optional<PushField::Kind> kind = kinds[i];
            if (!kind)
                continue;
            // the node of an array element holds every element
            leaves[i].node->kind = *kind;
            leaves[i].node->access = MergeAccess(leaves[i].node->access, walk.access(i));
            fields.push_back({ std::move(leaves[i].name), leaves[i].offset, *kind, walk.access(i) });
        }
        std::ranges::sort(fields, {}, &PushResourceField::offset);
        return fields;
    }
#endif
} // namespace mythril
