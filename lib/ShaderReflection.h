#pragma once

#include "mythril/Shader.h"

#include <span>
#include <string>
#include <vector>

namespace mythril {
    class SpirvLayout;
    struct ReflectionResult {
        PipelineLayoutSignature pipelineLayoutSignature;
        SpecializationInfo specializationInfo;
        std::vector<ShaderEntryPoint> entryPoints;
    };

    inline constexpr const char* kVertexEntryPoint = "vs_main";
    inline constexpr const char* kFragmentEntryPoint = "fs_main";
    inline constexpr const char* kComputeEntryPoint = "cs_main";

    const char* ScalarKindToString(ScalarKind scalarKind);
    ReflectionResult ReflectSPIRV(const SpirvLayout& layout);
#ifdef DEBUG
    // writes the kind and access of each pointer and handle into the blocks and returns them as leaves
    std::vector<PushResourceField> AnalyzePushResources(const SpirvLayout& layout, std::span<PushBlockLayout> blocks);
#endif
    inline PushField::Access MergeAccess(const PushField::Access a, const PushField::Access b) {
        const auto merged = static_cast<uint8_t>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
        return merged & static_cast<uint8_t>(PushField::Access::Unknown) ? PushField::Access::Unknown : static_cast<PushField::Access>(merged);
    }
} // namespace mythril
