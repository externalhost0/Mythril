#include <doctest/doctest.h>
#include "test_fixtures.h"
#include "mythril/RenderGraphBuilder.h"
#include "Specs.h"
#include "Pipelines.h"

TEST_SUITE("Compute") {

TEST_CASE("compute dispatch") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/NoopCompute.slang",
        .debugName = "Compute Dispatch Shader",
    });
    REQUIRE(shader.valid());

    mythril::ComputePipeline pipeline = ctx.createComputePipeline({
        .shader = shader.handle(),
        .debugName = "Compute Dispatch Pipeline",
    });
    REQUIRE(pipeline.valid());

    mythril::Texture dummyTarget = ctx.createTexture({
        .dimension = {1, 1, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Storage,
        .debugName = "Compute Dummy",
    });

    mythril::RenderGraph graph;
    graph.addComputePass("compute")
        .dependency(dummyTarget, mythril::Layout::GENERAL)
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBindComputePipeline(pipeline);
            cmd.cmdDispatchThreadGroup({1, 1, 1});
        });

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

TEST_CASE("compute writes to storage buffer") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/SimpleCompute.slang",
        .debugName = "Compute Write Shader",
    });
    REQUIRE(shader.valid());

    mythril::ComputePipeline pipeline = ctx.createComputePipeline({
        .shader = shader.handle(),
        .debugName = "Compute Write Pipeline",
    });
    REQUIRE(pipeline.valid());

    mythril::Buffer outputBuf = ctx.createBuffer({
        .size = 64 * sizeof(uint32_t),
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::Device,
        .debugName = "Compute Output Buffer",
    });

    mythril::RenderGraph graph;
    graph.addComputePass("compute")
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBindComputePipeline(pipeline);
            struct { uint64_t addr; uint32_t count; uint32_t _pad; } push = {
                outputBuf.gpuAddress(), 64, 0
            };
            cmd.cmdPushConstants(&push, sizeof(push), 0);
            cmd.cmdDispatchThreadGroup({1, 1, 1});
        });

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

}
