cmake_minimum_required(VERSION 3.28)
project(mythril_samples LANGUAGES C CXX)

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
        "SDL_RENDER OFF"
        "SDL_GPU OFF"
        "SDL_CAMERA OFF"
        "SDL_OPENGL OFF"
        "SDL_OPENGLES OFF"
        "SDL_DIALOG OFF"
        "SDL_VIDEO_RENDER_VULKAN OFF"
)

# ==================== Sample Creation Macro ====================
# a plain path lets the build track the .spv files, multi-config generators need the per-config directory
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
# SHADERS, UNTYPED_SHADERS and MODULES are relative to the sample folder
macro(ADD_SAMPLE sample_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs LIBS SHADERS UNTYPED_SHADERS MODULES)
    cmake_parse_arguments(SAMPLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(sample_file "${sample_name}/main.cpp")
    add_executable(${sample_name} ${sample_file})

    target_link_libraries(${sample_name} PRIVATE mythril SDL3::SDL3 ${SAMPLE_LIBS})

    target_compile_definitions(${sample_name} PRIVATE MYTH_SAMPLE_NAME="${sample_name}")
    target_include_directories(${sample_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    foreach(feature IN LISTS SAMPLE_${sample_name}_REQUIRES)
        if(${feature})
            target_compile_definitions(${sample_name} PRIVATE ${feature}=1)
        endif()
    endforeach()

    file(GLOB_RECURSE _sample_assets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}/*")
    list(FILTER _sample_assets EXCLUDE REGEX "\\.(cpp|cxx|cc|c|slang)$")
    set(_assets_stamp "${CMAKE_CURRENT_BINARY_DIR}/${sample_name}_assets.stamp")
    add_custom_command(OUTPUT "${_assets_stamp}"
            COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}"
            -DDST_DIR="$<TARGET_FILE_DIR:${sample_name}>/${sample_name}_data"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake"
            COMMAND ${CMAKE_COMMAND} -E touch "${_assets_stamp}"
            DEPENDS ${_sample_assets} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake"
            COMMENT "Copying ${sample_name} assets to isolated runtime directory"
            COMMAND_EXPAND_LISTS
    )
    target_sources(${sample_name} PRIVATE "${_assets_stamp}")

    set_target_properties(${sample_name} PROPERTIES FOLDER "Samples")

    if(SAMPLE_SHADERS OR SAMPLE_UNTYPED_SHADERS)
        # the samples root holds shader_types.h, and each shader folder holds what its shaders import
        set(_search_paths "${CMAKE_CURRENT_SOURCE_DIR}")
        foreach(_kind SHADERS UNTYPED_SHADERS MODULES)
            set(_sample_${_kind})
            foreach(_file IN LISTS SAMPLE_${_kind})
                list(APPEND _sample_${_kind} "${sample_name}/${_file}")
                get_filename_component(_dir "${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}/${_file}" DIRECTORY)
                list(APPEND _search_paths "${_dir}")
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES _search_paths)
        if(_multi_config)
            set(_spirv_dir "$<TARGET_FILE_DIR:${sample_name}>/${sample_name}_data")
        else()
            set(_spirv_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${sample_name}_data")
        endif()
        mythril_reflect_shaders(${sample_name} SOURCES ${_sample_SHADERS} UNTYPED_SOURCES ${_sample_UNTYPED_SHADERS} MODULES ${_sample_MODULES} SEARCH_PATHS ${_search_paths}
                MATRIX_LAYOUT COLUMN TYPES_HEADER shader_types.h TYPES_NAMESPACE mythril::GPU SPIRV_DIR "${_spirv_dir}")
    endif()
endmacro()

# ==================== Sample Dependencies ====================
CPMAddPackage(
        NAME glm
        GITHUB_REPOSITORY "g-truc/glm"
        GIT_TAG 1.0.1
)
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)
add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

CPMAddPackage(
        NAME fastgltf
        GITHUB_REPOSITORY "spnda/fastgltf"
        GIT_TAG v0.9.0
)

CPMAddPackage(
        NAME stb_image
        GITHUB_REPOSITORY "nothings/stb"
        GIT_TAG master
)
if(stb_image_ADDED)
    add_library(stb_image INTERFACE IMPORTED)
    target_include_directories(stb_image INTERFACE ${stb_image_SOURCE_DIR})
endif()

# sample 7 and 9 both use it for interaction
CPMAddPackage(
        NAME imguizmo
        GITHUB_REPOSITORY "CedricGuillemet/ImGuizmo"
        GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
        DOWNLOAD_ONLY True
)
if(MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING)
    add_library(imgui_sdl3_backend STATIC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    )
    target_link_libraries(imgui_sdl3_backend PUBLIC imgui SDL3::SDL3)
endif()

if(imguizmo_ADDED AND (MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING))
    file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/src/*.cpp)
    file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/src/*.h)

    add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
    target_link_libraries(imguizmo PUBLIC imgui)
    target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR}/src)
endif()

# ==================== Sample Definitions ====================
# Only build samples whose required features the parent CMakeLists kept enabled.
set(_sample_args_01_ClearWindow                "")
set(_sample_args_02_Cube                       LIBS glm SHADERS BasicRed.slang)
set(_sample_args_03_CubeMSAA                   LIBS glm SHADERS BasicRed.slang)
set(_sample_args_04_PostProcessingCube         LIBS glm SHADERS BasicRed.slang FullscreenPost.slang)
set(_sample_args_05_ImGui                      LIBS imgui_sdl3_backend)
set(_sample_args_06_ComplexShader              LIBS glm imgui_sdl3_backend SHADERS Marble.slang)
set(_sample_args_07_CompleteScene              LIBS glm fastgltf stb_image imguizmo imgui_sdl3_backend
        SHADERS shaders/DepthOnly.slang shaders/RedDebug.slang shaders/FullscreenComposite.slang shaders/PointShadow.slang shaders/Standard.slang shaders/Sky.slang
        shaders/Particles.slang shaders/Downsample.slang shaders/ConversionLuminance.slang shaders/Upsample.slang shaders/Adaptation.slang
        MODULES shaders/GPUStructs.slang)
set(_sample_args_08_RayTracing                 LIBS glm fastgltf imgui_sdl3_backend
        SHADERS raytrace.slang Wind.slang Leaves.slang InstanceBounds.slang BoundsLines.slang MODULES Scene.slang)
set(_sample_args_09_Skinning                   LIBS glm fastgltf imguizmo imgui_sdl3_backend UNTYPED_SHADERS Skinning.slang)

foreach(sample IN LISTS MYTH_SAMPLES_TO_BUILD)
    ADD_SAMPLE(${sample} ${_sample_args_${sample}})
endforeach()
