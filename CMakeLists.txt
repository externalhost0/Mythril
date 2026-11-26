cmake_minimum_required(VERSION 3.28)
project(mythril VERSION 1.0.0 LANGUAGES C CXX)

# ==================== Output Directories ====================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# for mythril to be a dynamic library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ==================== User Options ====================
option(MYTH_BUILD_SAMPLES "Build sample applications" OFF)
option(MYTH_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(MYTH_ENABLE_IMGUI_STANDARD "Enable ImGui plugin (main branch)" OFF)
option(MYTH_ENABLE_IMGUI_DOCKING "Enable ImGui plugin (docking branch)" OFF)
option(MYTH_ENABLE_TRACY "Enable Tracy profiling" OFF)
option(MYTH_ENABLE_TRACY_GPU "Enable Tracy GPU profiling (EXPERIMENTAL)" OFF)
option(MYTH_ENABLE_TRACY_GPU_COMMANDS "Enable Tracy GPU per-command zones, dont use for measurement!" OFF)

# ==================== Sample Feature Requirements ====================
set(SAMPLE_01_ClearWindow_REQUIRES        "")
set(SAMPLE_02_Cube_REQUIRES               "")
set(SAMPLE_03_CubeMSAA_REQUIRES           "")
set(SAMPLE_04_PostProcessingCube_REQUIRES "")
set(SAMPLE_05_ImGui_REQUIRES              "MYTH_ENABLE_IMGUI_STANDARD")
set(SAMPLE_06_ComplexShader_REQUIRES      "MYTH_ENABLE_IMGUI_STANDARD")
set(SAMPLE_07_CompleteScene_REQUIRES      "MYTH_ENABLE_IMGUI_STANDARD;MYTH_ENABLE_TRACY")
set(SAMPLE_09_Skinning_REQUIRES           "MYTH_ENABLE_IMGUI_STANDARD")
set(ALL_SAMPLES
        01_ClearWindow
        02_Cube
        03_CubeMSAA
        04_PostProcessingCube
        05_ImGui
        06_ComplexShader
        07_CompleteScene
        08_RayTracing
        09_Skinning
)

# ==================== C++ Standard ====================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (APPLE)
    add_compile_definitions(_LIBCPP_REMOVE_TRANSITIVE_INCLUDES)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
# for msvc compat
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
set(CPM_USE_NAMED_CACHE_DIRECTORIES ON)
include(cmake/QuietDeps.cmake)
QUIET_DEPS_START()
include(cmake/CPM.cmake)
QUIET_DEPS_END()

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

#s setupid windows has to be different
if(VULKAN_SDK_DIR)
    if(WIN32)
        set(VULKAN_SDK_LIBDIR "${VULKAN_SDK_DIR}/Lib")
    else()
        set(VULKAN_SDK_LIBDIR "${VULKAN_SDK_DIR}/lib")
    endif()
endif()

# ==================== Slang Shader Compiler ====================
set(SLANG_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}/slang")
find_library(SLANG_LIBRARY
        NAMES slang
        PATHS "${VULKAN_SDK_LIBDIR}"
        REQUIRED
)
message(STATUS "Using Slang from Vulkan SDK: ${SLANG_LIBRARY}")

# ==================== Private CPM Dependencies ====================
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
    if(NOT TARGET imgui)
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

    target_link_libraries(imgui PRIVATE volk)
    endif ()

    set(MYTHRIL_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}" CACHE PATH "ImGui source directory (set by mythril)" FORCE)
endif()

# ==================== Tracy (Optional) ====================
if(MYTH_ENABLE_TRACY_GPU AND NOT MYTH_ENABLE_TRACY)
    message(FATAL_ERROR "MYTH_ENABLE_TRACY_GPU requires MYTH_ENABLE_TRACY=ON")
endif()
if(MYTH_ENABLE_TRACY_GPU_COMMANDS AND NOT MYTH_ENABLE_TRACY_GPU)
    message(FATAL_ERROR "MYTH_ENABLE_TRACY_GPU_COMMANDS requires MYTH_ENABLE_TRACY_GPU=ON")
endif()
if(MYTH_ENABLE_TRACY)
    CPMAddPackage(
            NAME tracy
            GITHUB_REPOSITORY "wolfpld/tracy"
            GIT_TAG "v0.13.1"
            PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/tracy-vk-query-count.patch"
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
        lib/RenderGraphDebugChecks.cpp
        lib/RenderGraphStructures.cpp
        lib/Shader.cpp
        lib/Objects.cpp
)

if(MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING)
    list(APPEND MYTHRIL_SOURCES lib/plugins/ImGuiPlugin.cpp)
endif()
if(MYTH_ENABLE_TRACY_GPU)
    list(APPEND MYTHRIL_SOURCES lib/plugins/TracyGPUPlugin.cpp)
endif()

# SPIR-V parsing shared by mythril and mythril-reflect, so the tool does not wait on the whole library
add_library(mythril-spirv STATIC lib/SpirvLayout.cpp)
target_include_directories(mythril-spirv PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
)
target_compile_definitions(mythril-spirv PUBLIC VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS)
target_link_libraries(mythril-spirv PUBLIC Vulkan::Headers volk::volk PRIVATE fmt::fmt)

add_library(mythril STATIC
        ${MYTHRIL_SOURCES}
)
# cause we are lazy
add_library(mythril::mythril ALIAS mythril)

target_include_directories(mythril
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

if(MYTH_ENABLE_TRACY)
    target_include_directories(mythril PUBLIC
            $<BUILD_INTERFACE:$<TARGET_PROPERTY:TracyClient,INTERFACE_INCLUDE_DIRECTORIES>>
    )
endif()

target_compile_definitions(mythril PUBLIC
        VK_NO_PROTOTYPES
        VK_ENABLE_BETA_EXTENSIONS
)

target_link_libraries(mythril
        PUBLIC
        Vulkan::Vulkan
        volk::volk
        PRIVATE
        mythril-spirv
        fmt::fmt
        GPUOpen::VulkanMemoryAllocator
)

# platform-specific linking
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(mythril PUBLIC
            -static
            -static-libgcc
            -static-libstdc++
    )
endif()


# ImGui integration
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

# Tracy integration
if(MYTH_ENABLE_TRACY)
    target_compile_definitions(mythril PUBLIC MYTH_ENABLED_TRACY)
    target_link_libraries(mythril PUBLIC $<BUILD_INTERFACE:TracyClient>)
    if(MYTH_ENABLE_TRACY_GPU)
        target_compile_definitions(mythril PUBLIC
                MYTH_ENABLED_TRACY_GPU
                TRACY_VK_USE_SYMBOL_TABLE
        )
        if(APPLE)
            # MoltenVK silently emulates timestamp pools larger than Metal's 32KB counter buffer limit
            target_compile_definitions(mythril PUBLIC TRACY_VK_QUERY_COUNT=4096)
        endif()
        if(MYTH_ENABLE_TRACY_GPU_COMMANDS)
            if(APPLE)
                message(WARNING "MYTH_ENABLE_TRACY_GPU_COMMANDS is ignored on Apple")
            endif()
            target_compile_definitions(mythril PUBLIC MYTH_ENABLED_TRACY_GPU_COMMANDS)
        endif()
    endif()
endif()

add_library(mythril-slang STATIC EXCLUDE_FROM_ALL
        lib/SlangCompiler.cpp
        lib/PushLayout.cpp
)
target_include_directories(mythril-slang PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${SLANG_INCLUDE_DIR}
)
target_link_libraries(mythril-slang PUBLIC mythril-spirv fmt::fmt "${SLANG_LIBRARY}")

add_executable(mythril-reflect EXCLUDE_FROM_ALL tools/mythril-reflect/main.cpp)
target_link_libraries(mythril-reflect PRIVATE mythril-slang)
include(cmake/MythrilReflect.cmake)

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

# ==================== Tests ====================
if(MYTH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
