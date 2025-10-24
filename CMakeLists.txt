cmake_minimum_required(VERSION 3.28)
set(PARENT_PROJECT_NAME mythril)
project(${PARENT_PROJECT_NAME} LANGUAGES C CXX)

# ------------------- IMPORTANT | User Options ---------------------------
# default options
option(MYTH_ENABLE_IMGUI "Enable ImGui Plugin" ON)
option(MYTH_RUN_SAMPLES "Enable sample apps" ON)


# -------------------- cmake related options ------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(VERSION_STRING ${PROJECT_VERSION})
    add_compile_definitions(NDEBUG)
elseif (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(VERSION_STRING ${PROJECT_VERSION}-Debug)
    add_compile_definitions(DEBUG)
endif ()

include(cmake/CPM.cmake)

# -------------------------- VULKAN --------------------------------

# link VULKAN SDK and the also installed Slang Shading Lang Binaries
find_package(Vulkan REQUIRED COMPONENTS)
if(NOT VULKAN_FOUND)
    message(FATAL_ERROR "Vulkan SDK not installed.")
endif()
# -------------------------- SLANG SHADERS ------------------------
set(SLANG_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}/slang")
set(SLANG_LIBRARY_DIR "${Vulkan_LIBRARY_DIR}")
find_library(SLANG_LIBRARY
        NAMES slang
        PATHS "${SLANG_LIBRARY_DIR}"
        REQUIRED
)
if(NOT SLANG_LIBRARY)
    message(FATAL_ERROR "Slang library not found! This should be included with your installation of Vulkan. Please install a newer version of Vulkan for development.")
endif()
add_library(Slang::slang UNKNOWN IMPORTED)
set_target_properties(Slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
)

# ----------------------------- VOLK --------------------------------
if (WIN32)
    set(VSD VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
    set(VSD VK_USE_PLATFORM_MACOS_MVK)
endif()
CPMAddPackage(
        NAME volk
        GITHUB_REPOSITORY "zeux/volk"
        GIT_TAG vulkan-sdk-1.4.328
        OPTIONS
        "VOLK_STATIC_DEFINES=${VSD}"
)
# ---------------------- VMA ------------------------------
CPMAddPackage(
        NAME vma
        GITHUB_REPOSITORY "GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator"
        GIT_TAG v3.3.0
)
# --------------------- Vk-Bootstrap ---------------------------
CPMAddPackage(
        NAME vk-bootstrap
        GITHUB_REPOSITORY  "charles-lunarg/vk-bootstrap"
        GIT_TAG main
)
# ------------------------ SDL3 ---------------------------------
CPMAddPackage(
        NAME SDL3
        GITHUB_REPOSITORY "libsdl-org/SDL"
        GIT_TAG release-3.2.24
        OPTIONS
        "SDL_SHARED=OFF"
        "SDL_STATIC=ON"
        "SDL_TEST_LIBRARY=OFF"
        "SDL_TESTS=OFF"
        "SDL_DISABLE_INSTALL=ON"
        "SDL_DISABLE_INSTALL_DOCS=ON"
        "SDL_INSTALL_TESTS=OFF"
)
# ------------------------ FMT ---------------------------------
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY "fmtlib/fmt"
        GIT_TAG 12.0.0
)

# ------------------------- IMGUI -----------------------------
CPMAddPackage(
        NAME imgui
        GITHUB_REPOSITORY "ocornut/imgui"
        GIT_TAG master
)
add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp # optionally comment this out
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
        ${SDL3_INCLUDE_DIRS}
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
        IMGUI_IMPL_VULKAN_USE_VOLK
        IMGUI_DEFINE_MATH_OPERATORS
        IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)
target_link_libraries(imgui PRIVATE volk SDL3::SDL3)
# --------------- connecting source and includes -----------------------

add_library(${PARENT_PROJECT_NAME} STATIC
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
        lib/plugins/ImGuiPlugin.cpp
)
target_include_directories(${PARENT_PROJECT_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
)
target_compile_definitions(${PARENT_PROJECT_NAME} PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(${PARENT_PROJECT_NAME}
        PUBLIC
        SDL3::SDL3
        fmt
        GPUOpen::VulkanMemoryAllocator
        volk
        vk-bootstrap
        Slang::slang
)
if(MYTH_ENABLE_IMGUI)
    target_compile_definitions(${PARENT_PROJECT_NAME}
            PUBLIC
            MYTH_ENABLED_IMGUI
    )
    target_link_libraries(${PARENT_PROJECT_NAME}
            PUBLIC
            imgui
    )
endif()

# -------------------- samples -----------------------------
if(MYTH_RUN_SAMPLES)
    add_subdirectory(samples)
endif()
