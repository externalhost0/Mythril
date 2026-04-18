#include <doctest/doctest.h>
#include "test_fixtures.h"
#include "Specs.h"

#include <cstring>
#include <vector>

TEST_SUITE("Resources") {

TEST_CASE("buffer create and RAII") {
    auto& ctx = getTestContext();
    mythril::Buffer buf = ctx.createBuffer({
        .size = 256,
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::HostVisible,
        .debugName = "Test Buffer",
    });
    CHECK(buf.valid());
    CHECK_FALSE(buf.empty());

    mythril::Buffer moved = std::move(buf);
    CHECK(moved.valid());
    CHECK(buf.empty());
}

TEST_CASE("buffer upload download round-trip") {
    auto& ctx = getTestContext();
    constexpr size_t kSize = 64;
    mythril::Buffer buf = ctx.createBuffer({
        .size = kSize,
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::HostVisible,
        .debugName = "Round-trip Buffer",
    });

    std::vector<uint8_t> src(kSize);
    for (size_t i = 0; i < kSize; i++) src[i] = static_cast<uint8_t>(i);

    ctx.upload(buf.handle(), src.data(), kSize, 0);

    std::vector<uint8_t> dst(kSize, 0);
    ctx.download(buf.handle(), dst.data(), kSize, 0);

    CHECK(std::memcmp(src.data(), dst.data(), kSize) == 0);
}

TEST_CASE("texture 2D creation") {
    auto& ctx = getTestContext();
    mythril::Texture tex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled | mythril::TextureUsageBits_Attachment,
        .debugName = "Test Texture",
    });
    CHECK(tex.valid());
}

TEST_CASE("sampler creation with non-default params") {
    auto& ctx = getTestContext();
    mythril::Sampler sampler = ctx.createSampler({
        .magFilter = mythril::SamplerFilter::Nearest,
        .minFilter = mythril::SamplerFilter::Nearest,
        .wrapU = mythril::SamplerWrap::ClampEdge,
        .wrapV = mythril::SamplerWrap::ClampEdge,
        .debugName = "Test Sampler",
    });
    CHECK(sampler.valid());
}

TEST_CASE("texture with mipmaps") {
    auto& ctx = getTestContext();
    mythril::Texture tex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled,
        .numMipLevels = 4,
        .debugName = "Mipmap Texture",
    });
    CHECK(tex.valid());
}

TEST_CASE("texture cube creation") {
    auto& ctx = getTestContext();
    mythril::Texture tex = ctx.createTexture({
        .dimension = {64, 64, 1},
        .type = mythril::TextureType::Type_Cube,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled,
        .numLayers = 6,
        .debugName = "Cube Texture",
    });
    CHECK(tex.valid());
}

TEST_CASE("texture 3D creation") {
    auto& ctx = getTestContext();
    mythril::Texture tex = ctx.createTexture({
        .dimension = {16, 16, 16},
        .type = mythril::TextureType::Type_3D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = mythril::TextureUsageBits_Sampled,
        .debugName = "3D Texture",
    });
    CHECK(tex.valid());
}

TEST_CASE("buffer with initial data on device") {
    auto& ctx = getTestContext();
    uint32_t data[] = {42, 99, 7, 256};
    mythril::Buffer buf = ctx.createBuffer({
        .size = sizeof(data),
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::Device,
        .initialData = data,
        .debugName = "InitData Buffer",
    });
    CHECK(buf.valid());
}

TEST_CASE("gpu address is nonzero for device buffer") {
    auto& ctx = getTestContext();
    mythril::Buffer buf = ctx.createBuffer({
        .size = 256,
        .usage = mythril::BufferUsageBits_Storage,
        .storage = mythril::StorageType::Device,
        .debugName = "GPU Address Buffer",
    });
    CHECK(buf.gpuAddress() != 0);
}

TEST_CASE("multiple sampler configurations") {
    auto& ctx = getTestContext();

    mythril::Sampler linear = ctx.createSampler({
        .magFilter = mythril::SamplerFilter::Linear,
        .minFilter = mythril::SamplerFilter::Linear,
        .mipMap = mythril::SamplerMipMap::Linear,
        .debugName = "Linear Sampler",
    });
    CHECK(linear.valid());

    mythril::Sampler nearest = ctx.createSampler({
        .magFilter = mythril::SamplerFilter::Nearest,
        .minFilter = mythril::SamplerFilter::Nearest,
        .mipMap = mythril::SamplerMipMap::Disabled,
        .wrapU = mythril::SamplerWrap::ClampEdge,
        .wrapV = mythril::SamplerWrap::ClampEdge,
        .wrapW = mythril::SamplerWrap::ClampEdge,
        .debugName = "Nearest Clamp Sampler",
    });
    CHECK(nearest.valid());

    mythril::Sampler depthSampler = ctx.createSampler({
        .depthCompareEnabled = true,
        .depthCompareOp = mythril::CompareOp::LessEqual,
        .debugName = "Depth Compare Sampler",
    });
    CHECK(depthSampler.valid());
}

TEST_CASE("shader compilation from slang file") {
    auto& ctx = getTestContext();
    mythril::Shader shader = ctx.createShader({
        .filePath = std::string(MYTH_TEST_SHADER_DIR) + "/Passthrough.slang",
        .debugName = "Passthrough Shader",
    });
    CHECK(shader.valid());
}

}
