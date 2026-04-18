project(mythril_tests LANGUAGES C CXX)

# ====================== doctest =========================
CPMAddPackage("gh:onqtam/doctest@2.4.12")

add_executable(mythril_tests
        test_main.cpp
        test_handle_pool.cpp
        test_specs.cpp
        test_context.cpp
        test_resources.cpp
        test_pipelines.cpp
        test_render_graph.cpp
        test_rendering.cpp
        test_compute.cpp
)

target_link_libraries(mythril_tests PRIVATE mythril doctest)

target_include_directories(mythril_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/lib
)

target_compile_definitions(mythril_tests PRIVATE
        MYTH_TEST_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        MYTH_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/include"
)

# ====================== CTest =============================
add_test(NAME mythril_all_tests COMMAND mythril_tests)
add_test(NAME layer0_handle_pool COMMAND mythril_tests --test-suite="HandlePool")
add_test(NAME layer0_specs COMMAND mythril_tests --test-suite="Specs")
add_test(NAME layer1_context COMMAND mythril_tests --test-suite="Context")
add_test(NAME layer1_resources COMMAND mythril_tests --test-suite="Resources")
add_test(NAME layer1_pipelines COMMAND mythril_tests --test-suite="Pipelines")
add_test(NAME layer2_render_graph COMMAND mythril_tests --test-suite="RenderGraph")
add_test(NAME layer2_rendering COMMAND mythril_tests --test-suite="Rendering")
add_test(NAME layer2_compute COMMAND mythril_tests --test-suite="Compute")
