cmake_minimum_required(VERSION 3.28)
project(mythril VERSION 1.0.0 LANGUAGES C CXX)

# ==================== Output Directories ====================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Ensure private deps merged into mythril.a are PIC-clean in case
# consumers link mythril into a shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ==================== User Options ====================
option(MYTH_BUILD_SAMPLES "Build sample applications" OFF)
option(MYTH_ENABLE_IMGUI_STANDARD "Enable ImGui plugin (main branch)" OFF)
option(MYTH_ENABLE_IMGUI_DOCKING "Enable ImGui plugin (docking branch)" OFF)
option(MYTH_ENABLE_TRACY "Enable Tracy profiling" OFF)
option(MYTH_ENABLE_TRACY_GPU "Enable Tracy GPU profiling (EXPERIMENTAL)" OFF)
option(MYTH_INSTALL "Generate install rules for mythril (EXPERIMENTAL: see README)" OFF)

# ==================== Sample Feature Requirements ====================
# Each entry lists CMake options that must be ON for that sample to build.
# Samples whose requirements aren't satisfied are skipped (with a STATUS log)
# rather than silently force-enabling features the user didn't ask for.
set(SAMPLE_01_ClearWindow_REQUIRES        "")
set(SAMPLE_02_Cube_REQUIRES               "")
set(SAMPLE_03_CubeMSAA_REQUIRES           "")
set(SAMPLE_04_PostProcessingCube_REQUIRES "")
set(SAMPLE_05_ImGui_REQUIRES              "MYTH_ENABLE_IMGUI_STANDARD")
set(SAMPLE_06_ComplexShader_REQUIRES      "MYTH_ENABLE_IMGUI_STANDARD")
set(SAMPLE_07_CompleteScene_REQUIRES      "MYTH_ENABLE_IMGUI_STANDARD;MYTH_ENABLE_TRACY")
set(ALL_SAMPLES
        01_ClearWindow
        02_Cube
        03_CubeMSAA
        04_PostProcessingCube
        05_ImGui
        06_ComplexShader
        07_CompleteScene
)

# ==================== C++ Standard ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:preprocessor /bigobj")
endif()

# ==================== Build Type Configuration ====================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(VERSION_STRING ${PROJECT_VERSION})
    add_compile_definitions(NDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(VERSION_STRING ${PROJECT_VERSION}-Debug)
    add_compile_definitions(DEBUG)
endif()

# ==================== CPM Package Manager ====================
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/cpm" CACHE STRING "CPM cache path")
set(CPM_USE_NAMED_CACHE_DIRECTORIES ON)
include(cmake/CPM.cmake)

# ==================== Vulkan SDK ====================
if(DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
elseif(APPLE)
    set(VULKAN_SDK_DIR "/usr/local")
endif()

find_package(Vulkan REQUIRED)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR "Vulkan SDK not installed. See README for setup.")
endif()

# Derive a library dir for slang lookup
if(VULKAN_SDK_DIR)
    if(WIN32)
        set(VULKAN_SDK_LIBDIR "${VULKAN_SDK_DIR}/Lib")
    else()
        set(VULKAN_SDK_LIBDIR "${VULKAN_SDK_DIR}/lib")
    endif()
endif()

# ==================== Slang Shader Compiler ====================
option(MYTH_USE_LOCAL_SLANG "Use a locally-built Slang instead of the Vulkan SDK's" OFF)
set(MYTH_LOCAL_SLANG_DIR "$ENV{HOME}/Libs/slang" CACHE PATH "Path to local Slang source")

if(MYTH_USE_LOCAL_SLANG)
    message(STATUS "Looking for local Slang at: ${MYTH_LOCAL_SLANG_DIR}")
    set(SLANG_BUILD_CONFIGS "Release" "Debug" "RelWithDebInfo")
    set(SLANG_LIBRARY_FOUND FALSE)
    foreach(CONFIG ${SLANG_BUILD_CONFIGS})
        if(APPLE)
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/libslang.dylib")
        elseif(WIN32)
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/bin/slang.dll")
            set(POTENTIAL_IMPLIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/slang.lib")
        else()
            set(POTENTIAL_LIB "${MYTH_LOCAL_SLANG_DIR}/build/${CONFIG}/lib/libslang.so")
        endif()
        set(POTENTIAL_INCLUDE "${MYTH_LOCAL_SLANG_DIR}")
        if(EXISTS "${POTENTIAL_LIB}")
            set(SLANG_LIBRARY "${POTENTIAL_LIB}")
            set(SLANG_INCLUDE_DIR "${POTENTIAL_INCLUDE}")
            set(SLANG_LIBRARY_FOUND TRUE)
            message(STATUS "Found local Slang (${CONFIG}): ${SLANG_LIBRARY}")
            break()
        endif()
    endforeach()
    if(NOT SLANG_LIBRARY_FOUND)
        message(FATAL_ERROR
                "Local Slang not found at ${MYTH_LOCAL_SLANG_DIR}/build/. "
                "Build it first or unset MYTH_USE_LOCAL_SLANG.")
    endif()
else()
    set(SLANG_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}/slang")
    find_library(SLANG_LIBRARY
            NAMES slang
            PATHS "${VULKAN_SDK_LIBDIR}"
            REQUIRED
    )
    message(STATUS "Using Slang from Vulkan SDK: ${SLANG_LIBRARY}")
endif()

# ==================== Private CPM Dependencies ====================
CPMAddPackage(
        NAME SPIRV-Reflect
        GITHUB_REPOSITORY "KhronosGroup/SPIRV-Reflect"
        GIT_TAG vulkan-sdk-1.4.335.0
        OPTIONS "SPIRV_REFLECT_STATIC_LIB ON"
)
add_library(SPIRV-Reflect-objects OBJECT
        ${SPIRV-Reflect_SOURCE_DIR}/spirv_reflect.c
)
target_include_directories(SPIRV-Reflect-objects PUBLIC
        ${SPIRV-Reflect_SOURCE_DIR}
)

CPMAddPackage(
        NAME volk
        GITHUB_REPOSITORY "zeux/volk"
        GIT_TAG vulkan-sdk-1.4.328
)

CPMAddPackage(
        NAME VulkanMemoryAllocator
        GITHUB_REPOSITORY "GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator"
        GIT_TAG v3.3.0
)

CPMAddPackage(
        NAME vk-bootstrap
        GITHUB_REPOSITORY "charles-lunarg/vk-bootstrap"
        GIT_TAG v1.4.335
)

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
    # ImGui's SDL3 backend transitively needs SDL3 headers; samples are the
    # canonical SDL3 consumer, so we defer fetching SDL3 to samples/ but
    # imgui+backend still needs to compile when samples are off (someone
    # might wire imgui through a non-SDL window). To keep the imgui plugin
    # buildable in lib-only mode, we fetch SDL3 headers here only when needed.
    if(NOT TARGET SDL3::SDL3)
        CPMAddPackage(
                NAME SDL3
                GITHUB_REPOSITORY "libsdl-org/SDL"
                GIT_TAG release-3.4.0
                OPTIONS
                "SDL_SHARED ON"
                "SDL_STATIC OFF"
                "SDL_TEST_LIBRARY OFF"
                "SDL_TESTS OFF"
                "SDL_DISABLE_INSTALL ON"
                "SDL_DISABLE_INSTALL_DOCS ON"
                "SDL_INSTALL_TESTS OFF"
        )
    endif()

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

# ==================== Tracy (Optional) ====================
if(MYTH_ENABLE_TRACY_GPU AND NOT MYTH_ENABLE_TRACY)
    message(FATAL_ERROR "MYTH_ENABLE_TRACY_GPU requires MYTH_ENABLE_TRACY=ON")
endif()
if(MYTH_ENABLE_TRACY)
    CPMAddPackage(
            NAME tracy
            GITHUB_REPOSITORY "wolfpld/tracy"
            GIT_TAG "v0.13.1"
            OPTIONS
            "TRACY_ENABLE=ON"
    )
endif()

# ==================== Mythril Library ====================
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
        lib/Shader.cpp
        lib/SlangCompiler.cpp
        lib/Objects.cpp
)

if(MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING)
    list(APPEND MYTHRIL_SOURCES lib/plugins/ImGuiPlugin.cpp)
endif()
if(MYTH_ENABLE_TRACY_GPU)
    list(APPEND MYTHRIL_SOURCES lib/plugins/TracyGPUPlugin.cpp)
endif()

# Bundle private static-dep objects directly into libmythril.a so consumers
# only need to link mythril (plus the publicly-required Vulkan + volk).
add_library(mythril STATIC
        ${MYTHRIL_SOURCES}
        $<TARGET_OBJECTS:SPIRV-Reflect-objects>
        $<TARGET_OBJECTS:fmt>
        $<TARGET_OBJECTS:vk-bootstrap>
)
add_library(mythril::mythril ALIAS mythril)

target_include_directories(mythril
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

# De-facto public header dependencies. The library reaches into fmt (via
# HelperMacros.h's assert macros) and VMA (via VulkanObjects.h) through
# inline method bodies in headers included by CTXBuilder.h — so consumers
# must see those headers at compile time. Their *objects* are still merged
# into libmythril.a (fmt) or header-only (VMA), so no separate link step is
# required.
target_include_directories(mythril PUBLIC
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:fmt,INTERFACE_INCLUDE_DIRECTORIES>>
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:GPUOpen::VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>>
)
if(MYTH_ENABLE_TRACY)
    target_include_directories(mythril PUBLIC
            $<BUILD_INTERFACE:$<TARGET_PROPERTY:TracyClient,INTERFACE_INCLUDE_DIRECTORIES>>
    )
endif()

# Build-only include propagation for the remaining merged objects.
target_include_directories(mythril PRIVATE
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:SPIRV-Reflect-objects,INTERFACE_INCLUDE_DIRECTORIES>>
        $<BUILD_INTERFACE:$<TARGET_PROPERTY:vk-bootstrap,INTERFACE_INCLUDE_DIRECTORIES>>
        $<BUILD_INTERFACE:${SLANG_INCLUDE_DIR}>
)

target_compile_definitions(mythril PUBLIC
        VK_NO_PROTOTYPES
        VK_ENABLE_BETA_EXTENSIONS
)

# Public: Vulkan + volk (real header dependency in include/mythril/*.h).
# Slang is needed at consumer link time but its location is found at
# install time by mythrilConfig.cmake; here we scope it to BUILD only.
target_link_libraries(mythril
        PUBLIC
        Vulkan::Vulkan
        volk::volk
        PRIVATE
        "$<BUILD_INTERFACE:${SLANG_LIBRARY}>"
)

# Platform-specific linking
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(mythril PUBLIC
            -static
            -static-libgcc
            -static-libstdc++
    )
endif()

# Help the dynamic loader find libslang at runtime on Linux when consumers
# don't explicitly set LD_LIBRARY_PATH (Vulkan SDK installs slang alongside).
if(UNIX AND NOT APPLE AND VULKAN_SDK_LIBDIR)
    set_target_properties(mythril PROPERTIES
            INSTALL_RPATH "${VULKAN_SDK_LIBDIR}"
    )
endif()

# ImGui integration. imgui (and its SDL3-shared dep) come from CPM and have
# no install rules of their own, so we scope them to BUILD_INTERFACE — they
# stay out of mythril's exported target set when MYTH_INSTALL=ON.
if(MYTH_ENABLE_IMGUI_STANDARD)
    target_compile_definitions(mythril PUBLIC MYTH_ENABLED_IMGUI)
    target_link_libraries(mythril PUBLIC $<BUILD_INTERFACE:imgui>)
elseif(MYTH_ENABLE_IMGUI_DOCKING)
    target_compile_definitions(mythril PUBLIC
            MYTH_ENABLED_IMGUI
            MYTH_ENABLED_IMGUI_DOCKING
    )
    target_link_libraries(mythril PUBLIC $<BUILD_INTERFACE:imgui>)
endif()

# Tracy integration (same BUILD_INTERFACE scoping rationale as imgui above).
if(MYTH_ENABLE_TRACY)
    target_compile_definitions(mythril PUBLIC MYTH_ENABLED_TRACY)
    target_link_libraries(mythril PUBLIC $<BUILD_INTERFACE:TracyClient>)
    if(MYTH_ENABLE_TRACY_GPU)
        if(APPLE)
            message(FATAL_ERROR "MYTH_ENABLE_TRACY_GPU is not supported on Apple platforms.")
        endif()
        target_compile_definitions(mythril PUBLIC
                MYTH_ENABLED_TRACY_GPU
                TRACY_VK_USE_SYMBOL_TABLE
        )
    endif()
endif()

# ==================== Samples ====================
if(MYTH_BUILD_SAMPLES)
    set(_samples_to_build "")
    foreach(sample IN LISTS ALL_SAMPLES)
        set(_skip FALSE)
        set(_missing "")
        foreach(feature IN LISTS SAMPLE_${sample}_REQUIRES)
            if(NOT ${feature})
                set(_skip TRUE)
                list(APPEND _missing ${feature})
            endif()
        endforeach()
        if(_skip)
            message(STATUS "[Samples] Skipping ${sample} (requires: ${_missing})")
        else()
            list(APPEND _samples_to_build ${sample})
        endif()
    endforeach()
    set(MYTH_SAMPLES_TO_BUILD "${_samples_to_build}" CACHE INTERNAL "")
    add_subdirectory(samples)
endif()

# ==================== Install Rules ====================
if(MYTH_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS mythril volk
            EXPORT mythrilTargets
            LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # imgui and TracyClient come from CPM and are scoped to BUILD_INTERFACE
    # above, so they intentionally do not appear in the install export.

    # The public headers reference private headers via "../../lib/Foo.h" and
    # the private headers reach back to public via "../include/mythril/Foo.h".
    # Those relative paths only resolve at build time. We rewrite them to a
    # uniform "internal/" / "../" layout in a staging dir before installing.
    set(_myth_install_stage "${CMAKE_CURRENT_BINARY_DIR}/install-stage")
    file(REMOVE_RECURSE "${_myth_install_stage}")

    file(GLOB _myth_public_hdrs RELATIVE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/mythril"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/mythril/*.h")
    foreach(_h ${_myth_public_hdrs})
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/mythril/${_h}" _content)
        string(REPLACE "../../lib/" "internal/" _content "${_content}")
        file(WRITE "${_myth_install_stage}/mythril/${_h}" "${_content}")
    endforeach()

    file(GLOB _myth_internal_hdrs RELATIVE
            "${CMAKE_CURRENT_SOURCE_DIR}/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.h")
    foreach(_h ${_myth_internal_hdrs})
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lib/${_h}" _content)
        string(REPLACE "../include/mythril/" "../" _content "${_content}")
        file(WRITE "${_myth_install_stage}/mythril/internal/${_h}" "${_content}")
    endforeach()

    install(DIRECTORY "${_myth_install_stage}/mythril"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
    )

    # Ship volk headers so consumers compiling against mythril's PUBLIC
    # interface can include <volk.h> without a separate find_package(volk).
    install(FILES "${volk_SOURCE_DIR}/volk.h"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT mythrilTargets
            FILE mythrilTargets.cmake
            NAMESPACE mythril::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mythril
    )

    configure_package_config_file(
            cmake/mythrilConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/mythrilConfig.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mythril
    )
    write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/mythrilConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/mythrilConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/mythrilConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mythril
    )
endif()
