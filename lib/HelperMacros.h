//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include <csignal>
#include <fmt/printf.h>
#include <source_location>

// when not in DEBUG we do not delete x unlike the other macros, as this includes actual vulkan calls
#ifdef DEBUG
#define VK_CHECK(x) \
    do { \
        VkResult errorResult = x; \
        if (errorResult) { \
            fmt::print(stderr, "[VULKAN] Detected Vulkan error: {}:{} -> {} \n\t", __FILE__, __LINE__, __func__); \
            std::raise(SIGABRT); \
        } \
    } while (0)
#else
#define VK_CHECK(x) x
#endif

// this macro is not meant to be used outside of this header file
#if defined(_MSC_VER)
#define ASSUME_(cond) __assume(cond)
#elif defined(__clang__) || defined(__GNUC__)
#define ASSUME_(cond) if (!(cond)) __builtin_unreachable()
#else
#define ASSUME_(cond) ((void)0)
#endif

#ifdef DEBUG
// DO NOT PASS SIDE EFFECT FUNCTIONS
#define ASSERT_MSG(ERROR, FORMAT, ...) do { \
    if (!static_cast<bool>(ERROR)) [[unlikely]] {                         \
        fmt::print(stderr, "[ASSERT_MSG] | {}:{} -> {} -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, __func__ __VA_OPT__(,) __VA_ARGS__); \
        std::raise(SIGABRT); \
    } \
    ASSUME_(ERROR); \
} while(0)
#else
#define ASSERT_MSG(ERROR, FORMAT, ...) ((void)0)
#endif

#ifdef DEBUG
// DO NOT PASS SIDE EFFECT FUNCTIONS
#define ASSERT_MSG_NOSOURCE(ERROR, FORMAT, ...) do { \
    if (!static_cast<bool>(ERROR)) [[unlikely]] {                         \
        fmt::print(stderr, "[ASSERT_MSG] | {} -> ERROR:\n" FORMAT "\n", __func__ __VA_OPT__(,) __VA_ARGS__); \
        std::raise(SIGABRT); \
    } \
    ASSUME_(ERROR); \
} while(0)
#else
#define ASSERT_MSG_NOSOURCE(ERROR, FORMAT, ...) ((void)0)
#endif


#ifdef DEBUG
#define ASSERT(ERROR) do { \
    if (!static_cast<bool>(ERROR)) [[unlikely]] {        \
        fmt::print(stderr, "[ASSERT] | {}:{} -> {} -> Expression: " #ERROR "\n", __FILE__, __LINE__, __func__); \
        std::raise(SIGABRT); \
    } \
    ASSUME_(ERROR); \
} while(0)
#else
#define ASSERT(ERROR) ((void)0)
#endif
