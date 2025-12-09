project(${PARENT_PROJECT_NAME}_Samples LANGUAGES C CXX)

macro(ADD_SAMPLE sample_name)
    set(options) # no ON/OFF flags here
    set(oneValueArgs)
    set(multiValueArgs LIBS OPTIONS)
    cmake_parse_arguments(SAMPLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    set(sample_file "${sample_name}/main.cpp")

    # cause cmake uses cache and we have to overwrite it
    if("MYTH_ENABLE_IMGUI_STANDARD=ON" IN_LIST SAMPLE_OPTIONS)
        set(MYTH_ENABLE_IMGUI_STANDARD ON CACHE BOOL "" FORCE)
    endif()

    add_executable(${sample_name} ${sample_file})
    target_link_libraries(${sample_name} PUBLIC
            ${PARENT_PROJECT_NAME}
            ${SAMPLE_LIBS}
    )
    # apply compile definitions
    foreach(opt IN LISTS SAMPLE_OPTIONS)
        target_compile_definitions(${sample_name} PRIVATE ${opt})
    endforeach()

    # super lazy fix as of now, just copies the assets into the build directory
    # glob and depend so that it runs every time
    file(GLOB_RECURSE SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")

    add_custom_command(TARGET ${sample_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/${sample_name}/"
            "$<TARGET_FILE_DIR:${sample_name}>/"
            DEPENDS ${SHADER_FILES}
    )

    # for editing of shaders, as they are currently not hot reloadable
    option(FORCE_REBUILD_SAMPLES "Always recompile samples" ON)
    if(FORCE_REBUILD_SAMPLES)
        add_custom_command(
                TARGET ${sample_name}
                PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Forcing rebuild for ${sample_name}"
                COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_SOURCE_DIR}/${sample_file}"
                COMMENT "Touching source to trigger rebuild"
        )
    endif()
endmacro()

# for some of the examples
CPMAddPackage(
        NAME glm
        GITHUB_REPOSITORY "g-truc/glm"
        GIT_TAG 1.0.1
)
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)
add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)
#add_compile_definitions(GLM_FORCE_LEFT_HANDED)

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
if (stb_image_ADDED)
    add_library(stb_image INTERFACE IMPORTED)
    target_include_directories(stb_image INTERFACE ${stb_image_SOURCE_DIR})
endif()

CPMAddPackage(
        NAME imguizmo
        GITHUB_REPOSITORY "CedricGuillemet/ImGuizmo"
        GIT_TAG master
        DOWNLOAD_ONLY True
)
if (imguizmo_ADDED)
    file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/*.cpp)
    file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/*.h)

    add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
    target_link_libraries(imguizmo PUBLIC imgui)
    target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR})
endif ()

ADD_SAMPLE("01_ClearWindow")
ADD_SAMPLE("02_Cube" LIBS glm)
ADD_SAMPLE("03_CubeMSAA" LIBS glm)
ADD_SAMPLE("04_PostProcessingCube" LIBS glm)
ADD_SAMPLE("05_ImGui" OPTIONS MYTH_ENABLE_IMGUI_STANDARD=ON)
ADD_SAMPLE("06_ComplexShader" LIBS glm OPTIONS MYTH_ENABLE_IMGUI_STANDARD=ON)
ADD_SAMPLE("07_HDR" LIBS glm fastgltf stb_image imguizmo OPTIONS MYTH_ENABLED_IMGUI_STANDARD=ON)