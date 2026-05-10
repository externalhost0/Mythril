cmake_minimum_required(VERSION 3.28)
project(mythril_samples LANGUAGES C CXX)

option(FORCE_REBUILD_SAMPLES "Always recompile samples" ON)

# Samples use SDL3 directly. The library no longer leaks SDL3 publicly, so we
# fetch it here. If a parent (e.g. the ImGui plugin path) already declared
# SDL3, we reuse that target instead of double-fetching.
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

# ==================== Sample Creation Macro ====================
macro(ADD_SAMPLE sample_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs LIBS)
    cmake_parse_arguments(SAMPLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(sample_file "${sample_name}/main.cpp")
    add_executable(${sample_name} ${sample_file})

    target_link_libraries(${sample_name} PRIVATE mythril SDL3::SDL3 ${SAMPLE_LIBS})

    target_compile_definitions(${sample_name} PRIVATE
        MYTH_SAMPLE_NAME="${sample_name}"
        MYTH_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/include"
    )

    foreach(feature IN LISTS SAMPLE_${sample_name}_REQUIRES)
        if(${feature})
            target_compile_definitions(${sample_name} PRIVATE ${feature}=1)
        endif()
    endforeach()

    add_custom_command(TARGET ${sample_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}"
            -DDST_DIR="$<TARGET_FILE_DIR:${sample_name}>/${sample_name}_data"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake"
            COMMENT "Copying ${sample_name} assets to isolated runtime directory"
    )

    if(FORCE_REBUILD_SAMPLES)
        add_custom_command(TARGET ${sample_name} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Forcing rebuild for ${sample_name}"
            COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_SOURCE_DIR}/${sample_file}"
            COMMENT "Touching source to trigger rebuild"
        )
    endif()

    set_target_properties(${sample_name} PROPERTIES FOLDER "Samples")
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

CPMAddPackage(
        NAME imguizmo
        GITHUB_REPOSITORY "CedricGuillemet/ImGuizmo"
        GIT_TAG master
        DOWNLOAD_ONLY True
)
if(imguizmo_ADDED AND (MYTH_ENABLE_IMGUI_STANDARD OR MYTH_ENABLE_IMGUI_DOCKING))
    file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/*.cpp)
    file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/*.h)

    add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
    target_link_libraries(imguizmo PUBLIC imgui)
    target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR})
endif()

# ==================== Sample Definitions ====================
# Only build samples whose required features the parent CMakeLists kept enabled.
set(_sample_args_01_ClearWindow        "")
set(_sample_args_02_Cube               "LIBS;glm")
set(_sample_args_03_CubeMSAA           "LIBS;glm")
set(_sample_args_04_PostProcessingCube "LIBS;glm")
set(_sample_args_05_ImGui              "")
set(_sample_args_06_ComplexShader      "LIBS;glm")
set(_sample_args_07_CompleteScene      "LIBS;glm;fastgltf;stb_image;imguizmo")

foreach(sample IN LISTS MYTH_SAMPLES_TO_BUILD)
    ADD_SAMPLE(${sample} ${_sample_args_${sample}})
endforeach()
