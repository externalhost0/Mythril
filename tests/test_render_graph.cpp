#include <doctest/doctest.h>
#include "test_fixtures.h"
#include "mythril/RenderGraphBuilder.h"
#include "Specs.h"

TEST_SUITE("RenderGraph") {

TEST_CASE("graphics pass with color attachment compiles") {
    auto& ctx = getTestContext();
    mythril::Texture colorTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "RG Color Target",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("clear")
        .attachment({
            .texDesc = colorTarget,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);
}

TEST_CASE("graphics pass with color and depth attachments compiles") {
    auto& ctx = getTestContext();
    mythril::Texture colorTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "RG Color",
    });
    mythril::Texture depthTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "RG Depth",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("main")
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
            .storeOp = mythril::StoreOp::NO_CARE,
        })
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);
}

TEST_CASE("MSAA with resolve target compiles") {
    auto& ctx = getTestContext();
    mythril::Texture msaaTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = mythril::SampleCount::X4,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "MSAA Target",
    });
    mythril::Texture resolveTarget = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
        .debugName = "Resolve Target",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("msaa")
        .attachment({
            .texDesc = msaaTarget,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
            .resolveTexDesc = mythril::TextureDesc{resolveTarget},
        })
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);
}

TEST_CASE("multi-pass dependency tracking compiles") {
    auto& ctx = getTestContext();
    mythril::Texture sharedTex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
        .debugName = "Shared Dependency Tex",
    });
    mythril::Texture outputTex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Attachment,
        .debugName = "Output Tex",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("writer")
        .attachment({
            .texDesc = sharedTex,
            .clearValue = {1.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.addGraphicsPass("reader")
        .attachment({
            .texDesc = outputTex,
            .clearValue = {0.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .dependency(sharedTex, mythril::Layout::READ)
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.compile(ctx);
}

TEST_CASE("compute pass in render graph compiles") {
    auto& ctx = getTestContext();
    mythril::Texture storageTex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Storage,
        .debugName = "Compute Storage Tex",
    });

    mythril::RenderGraph graph;
    graph.addComputePass("compute")
        .dependency(storageTex, mythril::Layout::GENERAL)
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            // no-op compute pass for compilation test
        });

    graph.compile(ctx);
}

TEST_CASE("intermediate pass blit compiles") {
    auto& ctx = getTestContext();
    mythril::Texture src = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled | mythril::TextureUsageBits_Attachment,
        .debugName = "Blit Src",
    });
    mythril::Texture dst = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled | mythril::TextureUsageBits_Attachment,
        .debugName = "Blit Dst",
    });

    mythril::RenderGraph graph;
    graph.addGraphicsPass("fill")
        .attachment({
            .texDesc = src,
            .clearValue = {1.f, 0.f, 0.f, 1.f},
            .loadOp = mythril::LoadOp::CLEAR,
            .storeOp = mythril::StoreOp::STORE,
        })
        .setExecuteCallback([](mythril::CommandBuffer& cmd) {
            cmd.cmdBeginRendering();
            cmd.cmdEndRendering();
        });

    graph.addIntermediate("blit")
        .blit(src, dst)
        .finish();

    graph.compile(ctx);
}

}
