#pragma once

#include <cstddef>
#include <vector>

namespace layout {
    struct ClassLayout {
        const char* name;
        size_t size;
        size_t align;
    };
    std::vector<ClassLayout> DebugLayouts();
    std::vector<ClassLayout> ReleaseLayouts();
}
