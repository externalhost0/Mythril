#pragma once

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <format>

#ifdef DEBUG
#define SAMPLE_ASSERT(COND) do { \
    if (!static_cast<bool>(COND)) [[unlikely]] { \
        std::fputs(std::format("[SAMPLE_ASSERT] | {}:{} -> {} -> Expression: " #COND "\n", __FILE__, __LINE__, __func__).c_str(), stderr); \
        std::raise(SIGABRT); \
    } \
} while (0)

#define SAMPLE_ASSERT_MSG(COND, FORMAT, ...) do { \
    if (!static_cast<bool>(COND)) [[unlikely]] { \
        std::fputs(std::format("[SAMPLE_ASSERT_MSG] | {}:{} -> {} -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, __func__ __VA_OPT__(,) __VA_ARGS__).c_str(), stderr); \
        std::raise(SIGABRT); \
    } \
} while (0)
#else
#define SAMPLE_ASSERT(COND) ((void)0)
#define SAMPLE_ASSERT_MSG(COND, FORMAT, ...) ((void)0)
#endif

namespace sample {
	constexpr uint32_t CalcNumMipLevels(uint32_t width, uint32_t height) {
		uint32_t levels = 1;
		while ((width | height) >> levels)
			levels++;
		return levels;
	}
} // namespace sample
