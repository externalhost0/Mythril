cmake_minimum_required(VERSION 3.28)
project(mythril VERSION 1.0.0 LANGUAGES C CXX)

# ==================== Output Directories ====================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ==================== User Options ====================
option(MYTH_ENABLE_IMGUI_STANDARD "Enable ImGui Plugin Main Branch" OFF)
option(MYTH_ENABLE_IMGUI_DOCKING "Enable ImGui Plugin Docking Branch" OFF)
option(MYTH_ENABLE_TEST_CASES "Enable Test Cases (NOT DONE YET)" OFF)
option(MYTH_RUN_SAMPLES "Enable sample apps" ON)

# ==================== C++ Standard ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==================== Build Type Configuration ====================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(VERSION_STRING ${PROJECT_VERSION})
    add_compile_definitions(NDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(VERSION_STRING ${PROJECT_VERSION}-Debug)
    add_compile_definitions(DEBUG)
endif()

# ==================== CPM Package Manager ====================
include(cmake/CPM.cmake)

# ==================== Vulkan SDK ====================
if(WIN32)
    set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
elseif(APPLE)
    set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
    if(NOT VULKAN_SDK_DIR)
        set(VULKAN_SDK_DIR "/usr/local")
    endif()
elseif(UNIX)
    set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
endif()

if(NOT VULKAN_SDK_DIR)
    message(FATAL_ERROR "VULKAN_SDK environment variable not set/resolved!")
endif()

find_package(Vulkan REQUIRED)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR "Vulkan SDK not installed.")
endif()

# ==================== Slang Shader Compiler ====================
option(MYTH_USE_LOCAL_SLANG "Use local Slang build instead of Vulkan SDK version" OFF)
set(MYTH_LOCAL_SLANG_DIR "$ENV{HOME}/Libs/slang" CACHE PATH "Path to local Slang source directory")

if(MYTH_USE_LOCAL_SLANG)
    message(STATUS "Looking for local Slang at: ${MYTH_LOCAL_SLANG_DIR}")

    # Try to find built library in common configurations
    set(SLANG_BUILD_CONFIGS "Release" "Debug" "RelWithDebInfo")
    set(SLANG_LIBRARY_FOUND FALSE)

    foreach(CONFIG ${SLANG_BUILD_CONFIGS})
        if(APPLE)
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/libslang.dylib")
            set(POTENTIAL_INCLUDE "${MYTH_LOCAL_SLANG_DIR}")
        elseif(WIN32)
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/bin/slang.dll")
            set(POTENTIAL_IMPLIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/slang.lib")
            set(POTENTIAL_INCLUDE "${MYTH_LOCAL_SLANG_DIR}")
        else()
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/libslang.so")
            set(POTENTIAL_INCLUDE "${MYTH_LOCAL_SLANG_DIR}")
        endif()

        if(EXISTS "${POTENTIAL_LIB}")
            set(LOCAL_SLANG_LIBRARY "${POTENTIAL_LIB}")
            set(LOCAL_SLANG_INCLUDE "${POTENTIAL_INCLUDE}")
            set(SLANG_LIBRARY_FOUND TRUE)
            message(STATUS "Found local Slang (${CONFIG}): ${LOCAL_SLANG_LIBRARY}")
            break()
        endif()
    endforeach()

    if(NOT SLANG_LIBRARY_FOUND)
        message(FATAL_ERROR
                "Local Slang library not found at ${MYTH_LOCAL_SLANG_DIR}/build/\n"
                "Please build Slang first:\n"
                "  cd ${MYTH_LOCAL_SLANG_DIR}\n"
                "  cmake --workflow --preset release\n"
                "Or check that MYTH_LOCAL_SLANG_DIR points to the correct directory.")
    endif()

    add_library(slang SHARED IMPORTED)
    set_target_properties(slang PROPERTIES
            IMPORTED_LOCATION "${LOCAL_SLANG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LOCAL_SLANG_INCLUDE}"
    )

    # On Windows, also need the import library
    if(WIN32 AND POTENTIAL_IMPLIB AND EXISTS "${POTENTIAL_IMPLIB}")
        set_target_properties(slang PROPERTIES
                IMPORTED_IMPLIB "${POTENTIAL_IMPLIB}"
        )
    endif()
else()
    # Use Slang from Vulkan SDK (default)
    set(SLANG_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}/slang")
    if(WIN32)
        set(SLANG_LIBRARY_DIR "${VULKAN_SDK_DIR}/Lib")
    elseif(APPLE)
        set(SLANG_LIBRARY_DIR "${VULKAN_SDK_DIR}/lib")
    endif()

    find_library(SLANG_LIBRARY
            NAMES slang
            PATHS "${SLANG_LIBRARY_DIR}"
            REQUIRED
    )

    if(NOT SLANG_LIBRARY)
        message(FATAL_ERROR "Slang library not found! Please install a newer version of Vulkan SDK or enable MYTH_USE_LOCAL_SLANG.")
    endif()

    message(STATUS "Using Slang from Vulkan SDK: ${SLANG_LIBRARY}")

    add_library(slang UNKNOWN IMPORTED)
    set_target_properties(slang PROPERTIES
            IMPORTED_LOCATION "${SLANG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
    )
endif()

# ==================== SPIRV-Reflect ====================
CPMAddPackage(
        NAME SPIRV-Reflect
        GITHUB_REPOSITORY "KhronosGroup/SPIRV-Reflect"
        GIT_TAG vulkan-sdk-1.4.328
        OPTIONS "SPIRV_REFLECT_STATIC_LIB ON"
)

add_library(SPIRV-Reflect STATIC
        ${SPIRV-Reflect_SOURCE_DIR}/spirv_reflect.c
)
target_include_directories(SPIRV-Reflect PUBLIC
        ${SPIRV-Reflect_SOURCE_DIR}
)

# ==================== Volk (Vulkan Meta-Loader) ====================
if(WIN32)
    set(VOLK_PLATFORM_DEFINE VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
    set(VOLK_PLATFORM_DEFINE VK_USE_PLATFORM_METAL_EXT)
elseif(UNIX)
    set(VOLK_PLATFORM_DEFINE VK_USE_PLATFORM_XLIB_KHR)
endif()

CPMAddPackage(
        NAME volk
        GITHUB_REPOSITORY "zeux/volk"
        GIT_TAG vulkan-sdk-1.4.328
        OPTIONS "VOLK_STATIC_DEFINES ${VOLK_PLATFORM_DEFINE}"
)

# ==================== Vulkan Memory Allocator ====================
CPMAddPackage(
        NAME VulkanMemoryAllocator
        GITHUB_REPOSITORY "GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator"
        GIT_TAG v3.3.0
)

# ==================== Vk-Bootstrap ====================
CPMAddPackage(
        NAME vk-bootstrap
        GITHUB_REPOSITORY "charles-lunarg/vk-bootstrap"
        GIT_TAG main
)

# ==================== SDL3 ====================
CPMAddPackage(
        NAME SDL3
        GITHUB_REPOSITORY "libsdl-org/SDL"
        GIT_TAG release-3.2.24
        OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_TESTS OFF"
        "SDL_DISABLE_INSTALL ON"
        "SDL_DISABLE_INSTALL_DOCS ON"
        "SDL_INSTALL_TESTS OFF"
)

# ==================== fmt ====================
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY "fmtlib/fmt"
        GIT_TAG 12.0.0
)

# ==================== ImGui (Optional) ====================
if(MYTH_ENABLE_IMGUI_STANDARD AND MYTH_ENABLE_IMGUI_DOCKING)
    message(FATAL_ERROR "Cannot enable both MYTH_ENABLE_IMGUI_STANDARD and MYTH_ENABLE_IMGUI_DOCKING")
endif()

if(MYTH_ENABLE_IMGUI_STANDARD)
    set(IMGUI_GIT_TAG v1.92.4)
elseif(MYTH_ENABLE_IMGUI_DOCKING)
    set(IMGUI_GIT_TAG v1.92.4-docking)
endif()

if(MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING)
    CPMAddPackage(
            NAME imgui
            GITHUB_REPOSITORY "ocornut/imgui"
            GIT_TAG ${IMGUI_GIT_TAG}
    )

    add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    )

    target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )

    target_compile_definitions(imgui PUBLIC
            IMGUI_IMPL_VULKAN_USE_VOLK
            IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    )

    target_link_libraries(imgui PRIVATE volk SDL3::SDL3)
endif()

# ==================== My Library ====================
set(MYTHRIL_SOURCES
        lib/vkinfo.cpp
        lib/Swapchain.cpp
        lib/CTXBuilder.cpp
        lib/CTX.cpp
        lib/ImmediateCommands.cpp
        lib/StagingDevice.cpp
        lib/vkutil.cpp
        lib/vkimpl.cpp
        lib/CommandBuffer.cpp
        lib/VulkanObjects.cpp
        lib/PipelineBuilder.cpp
        lib/RenderGraphBuilder.cpp
        lib/Window.cpp
        lib/Shader.cpp
        lib/SlangCompiler.cpp
        lib/DescriptorWriter.cpp
)

if(MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING)
    list(APPEND MYTHRIL_SOURCES lib/plugins/ImGuiPlugin.cpp)
endif()

add_library(mythril STATIC ${MYTHRIL_SOURCES})

target_compile_options(mythril PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall>
        $<$<CXX_COMPILER_ID:MSVC>:
        /permissive->)

target_include_directories(mythril
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
)

target_compile_definitions(mythril PUBLIC
        VK_NO_PROTOTYPES
)

target_link_libraries(mythril
        PUBLIC
        SDL3::SDL3
        fmt::fmt
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        PRIVATE
        vk-bootstrap::vk-bootstrap
        SPIRV-Reflect
        slang
)

# Platform-specific linking
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(mythril PUBLIC
            -static
            -static-libgcc
            -static-libstdc++
    )
endif()

# ImGui integration
if(MYTH_ENABLE_IMGUI_STANDARD)
    target_compile_definitions(mythril PUBLIC
            MYTH_ENABLED_IMGUI
    )
    target_link_libraries(mythril PUBLIC imgui)
elseif(MYTH_ENABLE_IMGUI_DOCKING)
    target_compile_definitions(mythril PUBLIC
            MYTH_ENABLED_IMGUI
            MYTH_ENABLED_IMGUI_DOCKING
    )
    target_link_libraries(mythril PUBLIC imgui)
endif()

# ==================== Subdirectories ====================
if(MYTH_RUN_SAMPLES)
    add_subdirectory(samples)
endif()

if(MYTH_ENABLE_TEST_CASES)
    add_subdirectory(tests)
endif()