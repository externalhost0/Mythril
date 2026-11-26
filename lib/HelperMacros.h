//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include <csignal>
#include <cstdlib>
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
#define ASSUME_(cond) \
	if (!(cond)) __builtin_unreachable()
#else
#define ASSUME_(cond) ((void)0)
#endif

// asserts are debug only, release never evaluates or assumes the condition
#ifdef DEBUG
// DO NOT PASS SIDE EFFECT FUNCTIONS
#define ASSERT_MSG(ERROR, FORMAT, ...) \
	do { \
		if (!static_cast<bool>(ERROR)) [[unlikely]] { \
			fmt::print(stderr, "[ASSERT_MSG] | {}:{} -> {} -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__); \
			std::raise(SIGABRT); \
		} \
	} while (0)
// DO NOT PASS SIDE EFFECT FUNCTIONS, pass must be a PassDesc or CompiledPass
#define ASSERT_PASS(ERROR, PASS, FORMAT, ...) \
	do { \
		if (!static_cast<bool>(ERROR)) [[unlikely]] { \
			fmt::print(stderr, "[ASSERT_PASS] | {}:{} -> Pass '{}' -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, ::mythril::PassLogName(PASS) __VA_OPT__(, ) __VA_ARGS__); \
			std::raise(SIGABRT); \
		} \
	} while (0)
#define ASSERT(ERROR) \
	do { \
		if (!static_cast<bool>(ERROR)) [[unlikely]] { \
			fmt::print(stderr, "[ASSERT] | {}:{} -> {} -> Expression: " #ERROR "\n", __FILE__, __LINE__, __func__); \
			std::raise(SIGABRT); \
		} \
	} while (0)
// the only assumption, for code that cannot be reached, release reaching it is undefined behavior
#define UNREACHABLE(FORMAT, ...) \
	do { \
		fmt::print(stderr, "[UNREACHABLE] | {}:{} -> {} -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__); \
		std::raise(SIGABRT); \
		ASSUME_(false); \
	} while (0)
#else
// unevaluated, keeps variables used only by an assert from warning
#define ASSERT_MSG(ERROR, FORMAT, ...) ((void)sizeof(!(ERROR)))
#define ASSERT_PASS(ERROR, PASS, FORMAT, ...) ((void)sizeof(!(ERROR)))
#define ASSERT(ERROR) ((void)sizeof(!(ERROR)))
#define UNREACHABLE(FORMAT, ...) ASSUME_(false)
#endif

#define FATAL(FORMAT, ...) \
	do { \
		fmt::print(stderr, "[FATAL] | {}:{} -> {} -> MSG:\n" FORMAT "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__); \
		std::abort(); \
	} while (0)


#define MYTHRIL_INVALID_U8 ((uint8_t)0xFFu)
#define MYTHRIL_INVALID_U16 ((uint16_t)0xFFFFu)
#define MYTHRIL_INVALID_U32 0xFFFFFFFFu
#define MYTHRIL_INVALID_U64 0xFFFFFFFFFFFFFFFFull
