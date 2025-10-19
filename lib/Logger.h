//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once
#include <print>
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
constexpr const char* GetLogLevelAsString(LogType level) {
	return level_strings[(int)level];
}
#define LOG_EXCEPTION(exception) fmt::println(stderr, "[EXCEPTION] | {}", exception.what())
#define LOG_USER(level, message, ...) std::println("[{}] Source: \"{}\" | {}", GetLogLevelAsString(level), __PRETTY_FUNCTION__, std::format(message __VA_OPT__(, __VA_ARGS__)))
