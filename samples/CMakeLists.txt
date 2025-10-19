cmake_minimum_required(VERSION 3.28)
project(${PARENT_PROJECT_NAME}_Samples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

macro(ADD_SAMPLE sample_name)
    add_executable(${sample_name} "${sample_name}.cpp")
    target_link_libraries(${sample_name} PUBLIC
            ${PARENT_PROJECT_NAME}
            ${ARGN}
    )
    # super lazy fix as of now, just copies the assets into the build directory
    # glob and depend so that it runs every time
    file(GLOB_RECURSE SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")

    add_custom_command(TARGET ${sample_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:${sample_name}>/assets"
            DEPENDS ${SHADER_FILES}
    )
endmacro()

# for some of the examples
CPMAddPackage(
        NAME glm
        GITHUB_REPOSITORY "g-truc/glm"
        GIT_TAG 1.0.1
)
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)


ADD_SAMPLE("01_ClearWindow")
ADD_SAMPLE("02_Cube" glm)
ADD_SAMPLE("03_CubeMSAA" glm)
ADD_SAMPLE("04_PostProcessingCube" glm)
ADD_SAMPLE("05_ImGui")