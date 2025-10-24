//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include <fmt/printf.h>
#include <fmt/color.h>
#include <fmt/format.h>

namespace mythril {
	enum class LogType : uint8_t {
		Info = 0,
		Warning,
		Error,
		FatalError
	};
	constexpr const char* level_strings[] = {
			"Info",
			"Warning",
			"Error",
			"Fatal Error"
	};
	constexpr fmt::color level_colors[] = {
			fmt::color::antique_white,
			fmt::color::gold,
			fmt::color::red,
			fmt::color::magenta
	};
	constexpr const char* GetLogLevelAsString(LogType level) {
		return level_strings[(int) level];
	}
	constexpr fmt::color GetLogLevelAsColor(LogType level) {
		return level_colors[(int)level];
	}

#ifdef DEBUG
#define LOG_DEBUG(message, ...) fmt::print(fg(fmt::color::medium_spring_green), "[DEBUG] Source: \"{}\" | {}\n", __PRETTY_FUNCTION__, fmt::format(message __VA_OPT__(, __VA_ARGS__)))
#endif

#define LOG_USER(level, message, ...) fmt::print(fg(GetLogLevelAsColor(level)), "[{}] Source: \"{}\" | {}\n", GetLogLevelAsString(level), __PRETTY_FUNCTION__, fmt::format(message __VA_OPT__(, __VA_ARGS__)))
#define LOG_EXCEPTION(exception) fmt::println(stderr, "[EXCEPTION] | {}", exception.what())
}







