//
// Created by Hayden Rivas on 11/21/26.
//

#pragma once

#if defined(MYTH_ENABLED_TRACY_GPU)
#include <tracy/TracyVulkan.hpp>
#define MYTH_PROFILER_GPU_ZONE(ctx, name, cmdBuffer) TracyVkZoneTransient((ctx)->_tracyPlugin.getTracyVkCtx(), ___tracy_gpu_zone, cmdBuffer, name, true);
#else
#define MYTH_PROFILER_GPU_ZONE(ctx, name, cmdBuffer)
#endif // MYTH_ENABLED_TRACY_GPU

// metal only samples timestamps at encoder boundaries, per-command zones would split metal encoders
// aka you get horrible performance, so we disable it
#if defined(MYTH_ENABLED_TRACY_GPU_COMMANDS) && !defined(__APPLE__)
#define MYTH_PROFILER_GPU_COMMAND_ZONE(name, cmdBuffer, color) TracyVkZoneC(this->_ctx->_tracyPlugin.getTracyVkCtx(), cmdBuffer, name, color);
#else
#define MYTH_PROFILER_GPU_COMMAND_ZONE(name, cmdBuffer, color)
#endif
