#include <doctest/doctest.h>
#include "test_fixtures.h"

TEST_SUITE("Context") {

TEST_CASE("headless context creates successfully") {
    auto& ctx = getTestContext();
    (void)ctx;
    CHECK(true);
}

TEST_CASE("physical device properties are populated") {
    auto& ctx = getTestContext();
    auto props = ctx.getPhysicalDeviceProperties10();
    CHECK(props.apiVersion != 0);
    CHECK(props.deviceType != VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM);
}

TEST_CASE("vulkan 1.3 features available") {
    auto& ctx = getTestContext();
    auto features13 = ctx.getPhysicalDeviceFeatures13();
    CHECK(features13.dynamicRendering == VK_TRUE);
    CHECK(features13.synchronization2 == VK_TRUE);
}

}
