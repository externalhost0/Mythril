#include <doctest/doctest.h>
#include "Specs.h"

TEST_SUITE("Specs") {

TEST_CASE("Dimensions divide operations") {
    mythril::Dimensions d{100, 200, 300};

    auto d1 = d.divide1D(2);
    CHECK(d1.width == 50);
    CHECK(d1.height == 200);
    CHECK(d1.depth == 300);

    auto d2 = d.divide2D(2);
    CHECK(d2.width == 50);
    CHECK(d2.height == 100);
    CHECK(d2.depth == 300);

    auto d3 = d.divide3D(2);
    CHECK(d3.width == 50);
    CHECK(d3.height == 100);
    CHECK(d3.depth == 150);
}

TEST_CASE("Dimensions equality") {
    mythril::Dimensions a{10, 20, 30};
    mythril::Dimensions b{10, 20, 30};
    mythril::Dimensions c{10, 20, 31};

    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("ComponentMapping identity") {
    mythril::ComponentMapping m{};
    CHECK(m.identity());

    m.r = mythril::Swizzle_R;
    CHECK_FALSE(m.identity());
}

TEST_CASE("ComponentMapping toVkComponentMapping") {
    mythril::ComponentMapping m{
        .r = mythril::Swizzle_R,
        .g = mythril::Swizzle_G,
        .b = mythril::Swizzle_B,
        .a = mythril::Swizzle_A,
    };
    auto vk = m.toVkComponentMapping();
    CHECK(vk.r == VK_COMPONENT_SWIZZLE_R);
    CHECK(vk.g == VK_COMPONENT_SWIZZLE_G);
    CHECK(vk.b == VK_COMPONENT_SWIZZLE_B);
    CHECK(vk.a == VK_COMPONENT_SWIZZLE_A);
}

}
