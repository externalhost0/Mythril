#include <doctest/doctest.h>
#include "test_fixtures.h"
#include "Pipelines.h"

TEST_SUITE("Pipelines") {

TEST_CASE("graphics pipeline creation") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Pipeline Test Shader",
    });
    REQUIRE(shader.valid());

    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .topology = mythril::TopologyMode::TRIANGLE,
        .polygon = mythril::PolygonMode::FILL,
        .blend = mythril::BlendingMode::OFF,
        .cull = mythril::CullMode::OFF,
        .multisample = mythril::SampleCount::X1,
        .debugName = "Test Graphics Pipeline",
    });
    CHECK(pipeline.valid());
}

TEST_CASE("graphics pipeline with push constants") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/PushConstantTriangle.slang",
        .debugName = "PushConstant Shader",
    });
    REQUIRE(shader.valid());

    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .topology = mythril::TopologyMode::TRIANGLE,
        .polygon = mythril::PolygonMode::FILL,
        .blend = mythril::BlendingMode::OFF,
        .cull = mythril::CullMode::OFF,
        .multisample = mythril::SampleCount::X1,
        .debugName = "PushConstant Pipeline",
    });
    CHECK(pipeline.valid());
}

TEST_CASE("graphics pipeline move semantics") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Move Test Shader",
    });
    REQUIRE(shader.valid());

    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .debugName = "Move Test Pipeline",
    });
    CHECK(pipeline.valid());

    mythril::GraphicsPipeline moved = std::move(pipeline);
    CHECK(moved.valid());
    CHECK(pipeline.empty());
}

TEST_CASE("compute pipeline creation") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/SimpleCompute.slang",
        .debugName = "Compute Shader",
    });
    REQUIRE(shader.valid());

    mythril::ComputePipeline pipeline = ctx.createComputePipeline({
        .shader = shader.handle(),
        .debugName = "Test Compute Pipeline",
    });
    CHECK(pipeline.valid());
}

}
