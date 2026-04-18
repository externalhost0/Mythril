#include <doctest/doctest.h>
#include "test_fixtures.h"
#include "mythril/RenderGraphBuilder.h"
#include "Specs.h"
#include "Pipelines.h"

TEST_SUITE("Rendering") {

TEST_CASE("fullscreen triangle draw") {
    auto& ctx = getTestContext();
    mythril::Texture colorTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "Render Color Target",
    });

    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Fullscreen Shader",
    });
    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .topology = mythril::TopologyMode::TRIANGLE,
        .polygon = mythril::PolygonMode::FILL,
        .blend = mythril::BlendingMode::OFF,
        .cull = mythril::CullMode::OFF,
        .debugName = "Fullscreen Pipeline",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("draw")
        .attachment({
            .texDesc = colorTarget,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdBindGraphicsPipeline(pipeline);
            cmd.cmdDraw(3);
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

TEST_CASE("indexed draw with vertex buffer") {
    auto& ctx = getTestContext();
    mythril::Texture colorTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "Indexed Color Target",
    });

    struct Vertex { float pos[3]; };
    Vertex verts[] = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    uint32_t indices[] = {0, 1, 2};

    mythril::Buffer vertBuf = ctx.createBuffer({
        .size = sizeof(verts),
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::Device,
        .initialData = verts,
        .debugName = "Test Vertex Buffer",
    });
    mythril::Buffer idxBuf = ctx.createBuffer({
        .size = sizeof(indices),
        .usage = mythril::BufferUsageBits_Index,
        .storage = mythril::StorageType::Device,
        .initialData = indices,
        .debugName = "Test Index Buffer",
    });

    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/PushConstantTriangle.slang",
        .debugName = "Indexed Shader",
    });
    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .topology = mythril::TopologyMode::TRIANGLE,
        .polygon = mythril::PolygonMode::FILL,
        .blend = mythril::BlendingMode::OFF,
        .cull = mythril::CullMode::OFF,
        .debugName = "Indexed Pipeline",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("indexed")
        .attachment({
            .texDesc = colorTarget,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdBindGraphicsPipeline(pipeline);

            struct {
                float mvp[16];
                uint64_t vertexAddr;
            } push = {};
            // identity matrix
            push.mvp[0] = 1; push.mvp[5] = 1; push.mvp[10] = 1; push.mvp[15] = 1;
            push.vertexAddr = vertBuf.gpuAddress();
            cmd.cmdPushConstants(&push, sizeof(push), 0);

            cmd.cmdBindIndexBuffer(idxBuf);
            cmd.cmdDrawIndexed(3);
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

TEST_CASE("depth state binding") {
    auto& ctx = getTestContext();
    mythril::Texture colorTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "Depth Test Color",
    });
    mythril::Texture depthTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "Depth Test Depth",
    });

    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Depth Shader",
    });
    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .debugName = "Depth Pipeline",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("depth")
        .attachment({
            .texDesc = colorTarget,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .attachment({
            .texDesc = depthTarget,
            .clearValue = {1.f, 0u},
            .loadOp = mythril::LoadOp::CLEAR,
        })
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdBindGraphicsPipeline(pipeline);
            cmd.cmdBindDepthState({
                .compareOp = mythril::CompareOp::Less,
                .isDepthWriteEnabled = true,
            });
            cmd.cmdDraw(3);
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

TEST_CASE("multi-pass render graph executes") {
    auto& ctx = getTestContext();
    mythril::Texture target1 = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
        .debugName = "Multi Pass Target 1",
    });
    mythril::Texture target2 = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
        .debugName = "Multi Pass Target 2",
    });

    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Multi Pass Shader",
    });
    mythril::GraphicsPipeline pipeline = ctx.createGraphicsPipeline({
        .vertexShader = {shader},
        .fragmentShader = {shader},
        .debugName = "Multi Pass Pipeline",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("pass1")
        .attachment({
            .texDesc = target1,
            .clearValue = {1.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdBindGraphicsPipeline(pipeline);
            cmd.cmdDraw(3);
            cmd.cmdEndRendering();
        });

    graph.addGraphicsPass("pass2")
        .attachment({
            .texDesc = target2,
            .clearValue = {0.f, 0.f, 1.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .dependency(target1, mythril::Layout::READ)
        .setExecuteCallback([&](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdBindGraphicsPipeline(pipeline);
            cmd.cmdDraw(3);
            cmd.cmdEndRendering();
        });

    graph.addIntermediate("blit")
        .blit(target2, target1)
        .finish();

    graph.compile(ctx);

    mythril::CommandBuffer& cmd = ctx.acquireCommand(mythril::CommandBuffer::Type::General);
    graph.execute(cmd);
    ctx.submitCommand(cmd);
    CHECK(true);
}

}
