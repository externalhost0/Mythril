#include "layout/layout.h"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("public classes keep one layout whatever DEBUG the consumer sets") {
    const std::vector<layout::ClassLayout> debug = layout::DebugLayouts();
    const std::vector<layout::ClassLayout> release = layout::ReleaseLayouts();
    REQUIRE(debug.size() == 4);
    REQUIRE(release.size() == debug.size());
    // sizes only, a DEBUG member hidden in padding still shifts later offsets and passes here
    for (size_t i = 0; i < debug.size(); i++) {
        CAPTURE(std::string(debug[i].name));
        CHECK(debug[i].size == release[i].size);
        CHECK(debug[i].align == release[i].align);
    }
}
