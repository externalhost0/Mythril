#pragma once

#include "vkutil.h"
#include "vk_sync_tables.h"

#include <span>
#include <string>
#include <vector>

// the one spec checker every pure sync test uses: given accesses and the barriers placed between them, report what the spec says is wrong
namespace sync_spec {
    using mythril::StageAccess;
    using mythril::vkutil::BufferBarrier;
    using mythril::vkutil::BufferSyncFeatures;
    namespace st = mythril::sync_tables;

    template<size_t N>
    uint64_t Expand(const st::Entry (&table)[N], const uint64_t mask) {
        uint64_t out = 0;
        for (const st::Entry& e: table)
            if (mask & e.bit)
                out |= e.mask;
        return out;
    }
    inline uint64_t Stages(const VkPipelineStageFlags2 mask) { return Expand(st::kStageExpansion, mask); }
    inline uint64_t Access(const VkAccessFlags2 mask) { return Expand(st::kAccessExpansion, mask); }
    inline bool Contains(const uint64_t outer, const uint64_t inner) { return (inner & ~outer) == 0; }

    template<size_t N>
    std::string Names(const st::Entry (&table)[N], const uint64_t mask) {
        std::string out;
        for (const st::Entry& e: table)
            if (mask & e.bit)
                out += (out.empty() ? "" : "|") + std::string(e.name);
        return out.empty() ? "NONE" : out;
    }
    inline std::string StageNames(const VkPipelineStageFlags2 mask) { return Names(st::kStageExpansion, mask); }
    inline std::string AccessNames(const VkAccessFlags2 mask) { return Names(st::kAccessExpansion, mask); }

    // from the spec's access flag descriptions, never from vkutil::kWriteAccess
    constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    inline bool Writes(const StageAccess a) { return (Access(a.access) & Access(kWrites)) != 0; }

    // accesses inside one step are alternatives or one pass, so only pairs across steps are hazards; barriers in one step are one batch and never chain with each other
    struct Step {
        std::string name;
        std::vector<BufferBarrier> barriers;
        std::vector<StageAccess> accesses;
    };

    namespace detail {
        template<size_t N>
        uint64_t Known(const st::Entry (&table)[N]) {
            uint64_t out = 0;
            for (const st::Entry& e: table)
                out |= e.bit;
            return out;
        }

        inline void Valid(std::vector<std::string>& fail, const std::string& l, const StageAccess sa, const BufferSyncFeatures f) {
            if (!sa.stage)
                fail.push_back(l + ": empty stage mask");
            if (const uint64_t unknown = sa.stage & ~st::kAllStages)
                fail.push_back(l + ": unknown stage bits " + std::to_string(unknown));
            if (const uint64_t unknown = sa.access & ~(Known(st::kAccessExpansion) & Known(st::kAccessSupportedStages)))
                fail.push_back(l + ": unknown access bits " + std::to_string(unknown));
            if (sa.stage & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
                fail.push_back(l + ": ALL_COMMANDS is banned");
            if (sa.access & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT))
                fail.push_back(l + ": MEMORY_READ/MEMORY_WRITE is banned");
            if (!f.rayTracingPipeline && (sa.stage & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR))
                fail.push_back(l + ": ray tracing stage without the extension");
            if (!f.accelerationStructure && (sa.stage & (VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR)))
                fail.push_back(l + ": acceleration structure stage without the extension");
            const uint64_t stages = Stages(sa.stage);
            for (const st::Entry& e: st::kAccessSupportedStages)
                if ((sa.access & e.bit) && !(e.mask & stages))
                    fail.push_back(l + ": " + e.name + " not supported by " + StageNames(sa.stage));
        }

        // a meta bit like ALL_GRAPHICS counts as justified when it overlaps what is needed
        inline void Justified(std::vector<std::string>& fail, const std::string& l, const StageAccess sa, const uint64_t stages, const uint64_t access) {
            for (const st::Entry& e: st::kStageExpansion)
                if ((sa.stage & e.bit) && !(e.mask & stages))
                    fail.push_back(l + ": unjustified stage " + e.name);
            for (const st::Entry& e: st::kAccessExpansion)
                if ((sa.access & e.bit) && !(e.mask & access))
                    fail.push_back(l + ": unjustified access " + e.name);
        }

        inline std::string Describe(const std::string& step, const StageAccess a) { return step + " (" + StageNames(a.stage) + " / " + AccessNames(a.access) + ")"; }
    } // namespace detail

    // spec: a hazard pair is ordered by a chain of barriers in later steps, the first covering the earlier access in its src scope (and its writes),
    // each next one in a later step overlapping the previous dst stages, and the last covering the later access in its dst scope (and its access when the earlier one wrote)
    inline std::vector<std::string> Violations(const std::span<const Step> steps, const BufferSyncFeatures features) {
        std::vector<std::string> fail;
        const auto hazard = [](const StageAccess a, const StageAccess b) {
            return Writes(a) || Writes(b);
        };
        const auto label = [&](const size_t g, const size_t k) {
            return "barrier before " + steps[g].name + (steps[g].barriers.size() > 1 ? " #" + std::to_string(k) : std::string());
        };

        for (size_t g = 0; g < steps.size(); ++g) {
            // one barrier covers every access in its step once any of them has a hazard, since a batch cannot chain
            uint64_t srcStages = 0, srcAccess = 0, dstStages = 0, dstAccess = 0;
            for (const StageAccess j: steps[g].accesses) {
                for (size_t e = 0; e < g; ++e) {
                    for (const StageAccess i: steps[e].accesses) {
                        if (!hazard(i, j))
                            continue;
                        srcStages |= Stages(i.stage);
                        if (Writes(i))
                            srcAccess |= Access(i.access & kWrites);
                    }
                }
            }
            if (srcStages) {
                for (const StageAccess j: steps[g].accesses) {
                    dstStages |= Stages(j.stage);
                    dstAccess |= Access(j.access);
                }
            }
            for (size_t k = 0; k < steps[g].barriers.size(); ++k) {
                const BufferBarrier& b = steps[g].barriers[k];
                const std::string l = label(g, k);
                detail::Valid(fail, l + ".src", b.src, features);
                detail::Valid(fail, l + ".dst", b.dst, features);
                detail::Justified(fail, l + ".src", b.src, srcStages, srcAccess);
                detail::Justified(fail, l + ".dst", b.dst, dstStages, dstAccess);
            }
        }

        for (size_t a = 0; a < steps.size(); ++a) {
            for (const StageAccess i: steps[a].accesses) {
                for (size_t z = a + 1; z < steps.size(); ++z) {
                    for (const StageAccess j: steps[z].accesses) {
                        if (!hazard(i, j))
                            continue;
                        std::vector<std::vector<bool>> reached(steps.size());
                        bool ordered = false;
                        for (size_t g = a + 1; g <= z && !ordered; ++g) {
                            reached[g].assign(steps[g].barriers.size(), false);
                            for (size_t k = 0; k < steps[g].barriers.size() && !ordered; ++k) {
                                const BufferBarrier& b = steps[g].barriers[k];
                                bool start = Contains(Stages(b.src.stage), Stages(i.stage));
                                if (Writes(i))
                                    start = start && Contains(Access(b.src.access), Access(i.access & kWrites));
                                bool chained = false;
                                for (size_t m = a + 1; m < g; ++m)
                                    for (size_t n = 0; n < steps[m].barriers.size(); ++n)
                                        chained |= reached[m][n] && (Stages(steps[m].barriers[n].dst.stage) & Stages(b.src.stage)) != 0;
                                if (!start && !chained)
                                    continue;
                                reached[g][k] = true;
                                ordered = Contains(Stages(b.dst.stage), Stages(j.stage)) && (!Writes(i) || Contains(Access(b.dst.access), Access(j.access)));
                            }
                        }
                        if (!ordered)
                            fail.push_back(detail::Describe(steps[z].name, j) + " not ordered after " + detail::Describe(steps[a].name, i));
                    }
                }
            }
        }
        return fail;
    }
} // namespace sync_spec
