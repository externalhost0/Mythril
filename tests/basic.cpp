//
// Created by Hayden Rivas on 12/1/25.
//
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mythril/CTXBuilder.h"

class TestContextBuilder {
public:
	TestContextBuilder() {
		builder.set_info_spec({
			.app_name = "Test",
			.engine_name = "Test"
		})
		.set_window_spec({
			.title = "Test",
			.mode = mythril::WindowMode::Headless,
			.width = 64,
			.height = 64,
			.resizeable = false
		});
	}

	auto build() { return builder.build(); }
private:
	mythril::CTXBuilder builder;
};

TEST_CASE("Test") {
	auto ctx = TestContextBuilder{}.build();
}