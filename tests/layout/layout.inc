// both TUs see these headers under different DEBUG, so only take sizes here or the definitions break ODR
#include "layout.h"

#include <mythril/CTX.h>
#include <mythril/CommandBuffer.h>
#include <mythril/RenderGraphBuilder.h>
#include <mythril/Shader.h>

namespace layout {
    std::vector<ClassLayout> MYTH_LAYOUT_FN() {
        return {
            { "RenderGraph", sizeof(mythril::RenderGraph), alignof(mythril::RenderGraph) },
            { "CommandBuffer", sizeof(mythril::CommandBuffer), alignof(mythril::CommandBuffer) },
            { "detail::AllocatedShader", sizeof(mythril::detail::AllocatedShader), alignof(mythril::detail::AllocatedShader) },
            { "CTX", sizeof(mythril::CTX), alignof(mythril::CTX) },
        };
    }
}
