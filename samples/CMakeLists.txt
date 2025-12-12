cmake_minimum_required(VERSION 3.28)
project(mythril_samples LANGUAGES C CXX)

# ==================== Sample Creation Macro ====================
option(FORCE_REBUILD_SAMPLES "Always recompile samples" ON)

macro(ADD_SAMPLE sample_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs LIBS OPTIONS)
    cmake_parse_arguments(SAMPLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(sample_file "${sample_name}/main.cpp")

    # Handle ImGui cache override
    if("MYTH_ENABLE_IMGUI_STANDARD=ON" IN_LIST SAMPLE_OPTIONS)
        set(MYTH_ENABLE_IMGUI_STANDARD ON CACHE BOOL "" FORCE)
    endif()

    # Create executable
    add_executable(${sample_name} ${sample_file})

    # Link libraries
    target_link_libraries(${sample_name} PUBLIC
            mythril
            ${SAMPLE_LIBS}
    )

    # Apply compile definitions
    foreach(opt IN LISTS SAMPLE_OPTIONS)
        target_compile_definitions(${sample_name} PRIVATE ${opt})
    endforeach()

    # Copy all sample assets to executable directory
    # Glob to detect changes in assets
    file(GLOB_RECURSE SAMPLE_ASSET_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}/*")

    add_custom_command(TARGET ${sample_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}/"
            "$<TARGET_FILE_DIR:${sample_name}>/"
            DEPENDS ${SAMPLE_ASSET_FILES}
            COMMENT "Copying ${sample_name} assets to output directory"
    )

    # Force rebuild for shader editing
    if(FORCE_REBUILD_SAMPLES)
        add_custom_command(TARGET ${sample_name} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Forcing rebuild for ${sample_name}"
                COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_SOURCE_DIR}/${sample_file}"
                COMMENT "Touching source to trigger rebuild"
        )
    endif()

    # Organize in IDE
    set_target_properties(${sample_name} PROPERTIES FOLDER "Samples")
endmacro()

# ==================== Sample Dependencies ====================

# GLM - Mathematics library
CPMAddPackage(
        NAME glm
        GITHUB_REPOSITORY "g-truc/glm"
        GIT_TAG 1.0.1
)
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)
add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

# fastgltf - GLTF loader
CPMAddPackage(
        NAME fastgltf
        GITHUB_REPOSITORY "spnda/fastgltf"
        GIT_TAG v0.9.0
)

# stb_image - Image loader
CPMAddPackage(
        NAME stb_image
        GITHUB_REPOSITORY "nothings/stb"
        GIT_TAG master
)
if(stb_image_ADDED)
    add_library(stb_image INTERFACE IMPORTED)
    target_include_directories(stb_image INTERFACE ${stb_image_SOURCE_DIR})
endif()

# ImGuizmo - 3D gizmo for ImGui
CPMAddPackage(
        NAME imguizmo
        GITHUB_REPOSITORY "CedricGuillemet/ImGuizmo"
        GIT_TAG master
        DOWNLOAD_ONLY True
)
if(imguizmo_ADDED)
    file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/*.cpp)
    file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/*.h)

    add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
    target_link_libraries(imguizmo PUBLIC imgui)
    target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR})
endif()

# ==================== Sample Definitions ====================

ADD_SAMPLE("01_ClearWindow")
ADD_SAMPLE("02_Cube" LIBS glm)
ADD_SAMPLE("03_CubeMSAA" LIBS glm)
ADD_SAMPLE("04_PostProcessingCube" LIBS glm)
ADD_SAMPLE("05_ImGui" OPTIONS MYTH_ENABLE_IMGUI_STANDARD=ON)
ADD_SAMPLE("06_ComplexShader" LIBS glm OPTIONS MYTH_ENABLE_IMGUI_STANDARD=ON)
ADD_SAMPLE("07_HDR" LIBS glm fastgltf stb_image imguizmo OPTIONS MYTH_ENABLED_IMGUI_STANDARD=ON)