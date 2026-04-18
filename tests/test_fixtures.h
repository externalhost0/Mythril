#pragma once

#include "mythril/CTXBuilder.h"
#include <memory>

inline mythril::CTX& getTestContext() {
    static std::unique_ptr<mythril::CTX> ctx = []() {
        return mythril::CTXBuilder{}
            .set_vulkan_cfg({
                .app_name = "MythrilTests",
                .engine_name = "MythrilTestEngine",
                .enableValidation = true,
            })
            .set_slang_cfg({
                .searchpaths = { MYTH_INCLUDE_DIR, MYTH_TEST_SHADER_DIR },
            })
            .build();
    }();
    return *ctx;
}
