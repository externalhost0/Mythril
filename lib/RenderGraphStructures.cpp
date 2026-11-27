#include "RenderGraphInternal.h"
#include "HelperMacros.h"
#include "Logger.h"
#include "mythril/CTX.h"
#include "mythril/RenderGraphBuilder.h"

#include <algorithm>
#include <ranges>

namespace mythril {
    using namespace detail;

    GraphAccelerationStructure RenderGraph::importAccelerationStructure(const AccelerationStructure& structure) {
        ASSERT_MSG(structure.valid(), "RenderGraph::importAccelerationStructure called with an invalid acceleration structure.");
        const AllocatedAccelerationStructure& live = *structure.getContext()->_accelerationStructurePool.get(structure.handle());
        const GraphBuffer scratch = importBuffer(live._scratchBuffer);
        const GraphBuffer storage = importBuffer(live._buffer);
        GraphBufferResource& resource = _buffers[GraphBufferAccess::index(storage)];
        if (resource.structure)
            return GraphAccelerationStructureAccess::make(GraphBufferAccess::index(storage));
        GraphStructureSlot slot = {
            .handle = structure.handle(),
            .scratch = GraphBufferAccess::index(scratch),
            .topLevel = live._isTLAS,
            .allowsRefit = (live._spec.buildFlags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0,
        };
        for (const BufferHandle input: { live._spec.vertexBuffer, live._spec.indexBuffer, live._spec.transformBuffer.value_or(BufferHandle{}), live._spec.instancesBuffer })
            if (input.valid() && std::ranges::find(slot.inputs, input) == slot.inputs.end())
                slot.inputs.push_back(input);
        resource.debugName = live.getDebugName();
        resource.structure = std::move(slot);
        return GraphAccelerationStructureAccess::make(GraphBufferAccess::index(storage));
    }

    GraphBuffer RenderGraph::storageOf(const GraphAccelerationStructure structure) const {
        const uint32_t id = GraphAccelerationStructureAccess::index(structure);
        ASSERT_MSG(structure.valid() && id < _buffers.size() && _buffers[id].structure, "GraphAccelerationStructure does not belong to this RenderGraph.");
        return GraphBufferAccess::make(id, GraphAccelerationStructureAccess::version(structure));
    }

    GraphBuffer BasePassBuilder::declareOnce(const GraphBuffer buffer, const BufferAccess access) {
        const BufferHandle handle = _rGraph.handleOf(buffer);
        const bool declared = std::ranges::any_of(_passSource.bufferDependencyOperations, [&](const BufferDependencyDesc& dependency) {
            return dependency.handle == handle && dependency.access == access;
        });
        return declared ? buffer : declare(buffer, access);
    }
    GraphAccelerationStructure BasePassBuilder::dependency(const GraphAccelerationStructure structure, const AccelerationStructureAccess access) {
        ASSERT_PASS(access == AccelerationStructureAccess::Read, _passSource, "declares an acceleration structure with an unknown access.");
        const GraphBuffer storage = _rGraph.storageOf(structure);
        declare(storage, BufferAccess::AccelerationStructureRead);
        const std::optional<uint32_t> producer = _rGraph.producerOf(storage);
        if (!producer)
            return structure;
        // traversal also reads every BLAS the build of this TLAS version named
        const AccelerationStructureHandle handle = _rGraph._buffers[GraphBufferAccess::index(storage)].structure->handle;
        for (const TransferOp& op: _rGraph._passDescriptions[*producer].transferOps)
            if (const auto* build = std::get_if<AccelerationStructureBuildOp>(&op); build && build->structure == handle)
                for (const GraphBuffer blas: build->instanced)
                    declareOnce(blas, BufferAccess::AccelerationStructureRead);
        return structure;
    }

    GraphAccelerationStructure TransferBuilder::build(const GraphAccelerationStructure structure, const std::span<const GraphBuffer> inputs, const std::span<const GraphAccelerationStructure> instanced) {
        return declareBuild(structure, inputs, instanced, false);
    }
    GraphAccelerationStructure TransferBuilder::refit(const GraphAccelerationStructure structure, const std::span<const GraphBuffer> inputs, const std::span<const GraphAccelerationStructure> instanced) {
        return declareBuild(structure, inputs, instanced, true);
    }
    GraphAccelerationStructure TransferBuilder::declareBuild(const GraphAccelerationStructure structure, const std::span<const GraphBuffer> inputs, const std::span<const GraphAccelerationStructure> instanced, const bool refit) {
        RenderGraph& graph = base._rGraph;
        const PassDesc& pass = base._passSource;
        const GraphBuffer storage = graph.storageOf(structure);
        const GraphBufferResource& resource = graph._buffers[GraphBufferAccess::index(storage)];
        const GraphStructureSlot& slot = *resource.structure;
        const char* verb = refit ? "refit" : "build";
        ASSERT_PASS(!refit || slot.allowsRefit, pass, "refits '{}', which was not created with VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR. Add the flag or build it.", resource.debugName);
        ASSERT_PASS(std::ranges::none_of(pass.transferOps, [&](const TransferOp& op) {
            const auto* b = std::get_if<AccelerationStructureBuildOp>(&op);
            return b && b->structure == slot.handle;
        }),
            pass, "builds '{}' twice. Its scratch buffer is shared, so put the second {} in another pass.", resource.debugName, verb);
        for (const BufferHandle input: slot.inputs) {
            const auto imported = std::ranges::find(graph._buffers, input, &GraphBufferResource::handle);
            ASSERT_PASS(std::ranges::any_of(inputs, [&](const GraphBuffer buffer) {
                return graph.handleOf(buffer) == input;
            }),
                pass, "{}s '{}', which reads buffer '{}', but inputs does not name it, so nothing orders the build after its writes. Import it and pass it in inputs.", verb, resource.debugName, imported != graph._buffers.end() ? imported->debugName : std::string("(not imported)"));
        }
        for (const GraphBuffer buffer: inputs) {
            ASSERT_PASS(std::ranges::find(slot.inputs, graph.handleOf(buffer)) != slot.inputs.end(), pass, "{}s '{}' with input '{}', which it was not created from.", verb, resource.debugName, graph._buffers[GraphBufferAccess::index(buffer)].debugName);
            base.declareOnce(buffer, BufferAccess::AccelerationStructureBuildInput);
        }
        ASSERT_PASS(slot.topLevel || instanced.empty(), pass, "{}s BLAS '{}' with instanced structures, only a TLAS reads other structures.", verb, resource.debugName);
        std::vector<GraphBuffer> blases;
        for (const GraphAccelerationStructure blas: instanced) {
            const GraphBuffer blasStorage = graph.storageOf(blas);
            const uint32_t id = GraphBufferAccess::index(blasStorage);
            ASSERT_PASS(!graph._buffers[id].structure->topLevel, pass, "{}s TLAS '{}' over '{}', which is a TLAS. Instances point to BLASes.", verb, resource.debugName, graph._buffers[id].debugName);
            if (std::ranges::find(blases, blasStorage) == blases.end())
                blases.push_back(blasStorage);
            base.declareOnce(blasStorage, BufferAccess::AccelerationStructureBuildRead);
        }
        const uint32_t scratch = slot.scratch;
        base.declare(GraphBufferAccess::make(scratch, static_cast<uint32_t>(graph.producersOf(scratch).size())), BufferAccess::AccelerationStructureScratch);
        const GraphBuffer out = base.declare(storage, BufferAccess::AccelerationStructureBuildWrite);
        base._passSource.transferOps.emplace_back(AccelerationStructureBuildOp{ slot.handle, refit, std::move(blases) });
        return GraphAccelerationStructureAccess::make(GraphBufferAccess::index(out), GraphBufferAccess::version(out));
    }

#ifdef DEBUG
    // a pass records its ops with no barrier between them, so no buffer a build reads may be written in the same pass
    void CheckBuildHazards(const PassDesc& pass, const std::vector<GraphBufferResource>& buffers) {
        for (const BufferDependencyDesc& read: pass.bufferDependencyOperations) {
            if (read.access != BufferAccess::AccelerationStructureBuildInput && read.access != BufferAccess::AccelerationStructureBuildRead)
                continue;
            const bool written = std::ranges::any_of(pass.bufferDependencyOperations, [&](const BufferDependencyDesc& d) {
                return d.handle == read.handle && isWrite(d.access);
            });
            ASSERT_PASS(!written, pass, "builds from '{}', which this pass also writes. A pass has no barrier between its ops, so write or build it in an earlier pass.", buffers[GraphBufferAccess::index(read.buffer)].debugName);
        }
    }
#endif
} // namespace mythril
