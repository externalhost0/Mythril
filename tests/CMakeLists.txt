project(mythril_tests LANGUAGES CXX)

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "[Tests] Python 3 is needed to generate the sync tables, install Python 3 or configure with -DMYTH_BUILD_TESTS=OFF")
endif()

get_filename_component(_vk_include_parent "${Vulkan_INCLUDE_DIRS}" DIRECTORY)
find_file(MYTH_VK_XML vk.xml
        HINTS "${_vk_include_parent}/share/vulkan/registry" "${VULKAN_SDK_DIR}/share/vulkan/registry"
        NO_DEFAULT_PATH
        DOC "Vulkan registry vk.xml used to generate the sync tables for tests")
if(NOT MYTH_VK_XML)
    message(FATAL_ERROR "[Tests] vk.xml not found, set MYTH_VK_XML to the Vulkan SDK's share/vulkan/registry/vk.xml")
endif()

set(_gen_script "${CMAKE_CURRENT_SOURCE_DIR}/gen_sync_tables.py")
set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_gen_dir}")
execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_gen_script}" "${MYTH_VK_XML}" "${_gen_dir}/vk_sync_tables.h"
        RESULT_VARIABLE _gen_result
)
if(NOT _gen_result EQUAL 0)
    message(FATAL_ERROR "[Tests] gen_sync_tables.py failed on ${MYTH_VK_XML}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gen_script}" "${MYTH_VK_XML}")
message(STATUS "[Tests] sync tables generated from ${MYTH_VK_XML}")

CPMAddPackage(
        NAME doctest
        GITHUB_REPOSITORY "doctest/doctest"
        GIT_TAG v2.4.12
)

add_executable(mythril_tests
        main.cpp
        test_buffer_barriers.cpp
)
target_include_directories(mythril_tests PRIVATE "${_gen_dir}" "${mythril_SOURCE_DIR}/lib")
target_link_libraries(mythril_tests PRIVATE mythril doctest::doctest)

add_test(NAME mythril_tests COMMAND mythril_tests)
