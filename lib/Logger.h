//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include <iostream>
#include <fmt/printf.h>
#include <fmt/color.h>
#include <fmt/format.h>

namespace mythril {
	enum class LogType : uint8_t {
		Info = 0,
		Warning,
		Error,
		FatalError,
		Suggestion
	};
	constexpr const char* level_strings[] = {
			"Info",
			"Warning",
			"Error",
			"Fatal Error",
			"Suggestion"
	};
	constexpr uint32_t level_colors[] = {
			0xebeae4,
			0xfadc39,
			(uint32_t)fmt::color::orange_red,
			0xcf80ff,
			(uint32_t)fmt::color::cadet_blue
	};
	inline FILE* level_channels[] = {
		stdout,
		stdout,
		stderr,
		stderr,
		stdout,
	};
	constexpr const char* GetLogLevelAsString(LogType level) {
		return level_strings[(int)level];
	}
	constexpr uint32_t GetLogLevelAsColor(LogType level) {
		return level_colors[(int)level];
	}
	constexpr FILE* GetLogLevelAsFile(LogType level) {
		return level_channels[(int)level];
	}

// way to get namespaces and class prefixes without other stuff
#if defined(__clang__) || defined(__GNUC__)
	constexpr std::string_view StripPrettyFunction(std::string_view pretty) {
		const auto lparen = pretty.find('(');
		if (lparen == std::string_view::npos)
			return pretty;
		auto start = pretty.rfind(' ', lparen);
		start = (start == std::string_view::npos) ? 0 : start + 1;
		return pretty.substr(start, lparen - start);
	}
	#define EXPANDED_FUNCTION StripPrettyFunction(__PRETTY_FUNCTION__)
#elif defined(_MSC_VER)
	constexpr std::string_view StripFuncSig(std::string_view sig) {
		const auto lparen = sig.find('(');
		if (lparen == std::string_view::npos)
			return sig;
		auto start = sig.rfind(' ', lparen);
		start = (start == std::string_view::npos) ? 0 : start + 1;
		return sig.substr(start, lparen - start);
	}
	#define EXPANDED_FUNCTION StripFuncSig(__FUNCSIG__)
#else
	#define EXPANDED_FUNCTION __FUNCTION__
#endif


// less commonly placed, used more for user
#ifdef DEBUG
#define LOG_CUSTOM(file, color, message, ...) fmt::print(file, fg(color), "{}\n", fmt::format(message __VA_OPT__(, __VA_ARGS__)))
#else
#define LOG_CUSTOM(file, color, message, ...) ((void)0)
#endif

// commonly placed logging
#ifdef DEBUG
#define LOG_SYSTEM(level, message, ...) fmt::print(GetLogLevelAsFile(level), fg((fmt::color)GetLogLevelAsColor(level)), "[{}] Source: {} | {}\n", GetLogLevelAsString(level), EXPANDED_FUNCTION, fmt::format(message __VA_OPT__(, __VA_ARGS__)))
#else
#define LOG_SYSTEM(level, message, ...) (void(0))
#endif

#ifdef DEBUG
#define LOG_SYSTEM_NOSOURCE(level, message, ...) fmt::print(GetLogLevelAsFile(level), fg((fmt::color)GetLogLevelAsColor(level)), "[{}] {}\n", GetLogLevelAsString(level), fmt::format(message __VA_OPT__(, __VA_ARGS__)))
#else
#define LOG_SYSTEM_NOSOURCE(level, message, ...) (void(0))
#endif
}







