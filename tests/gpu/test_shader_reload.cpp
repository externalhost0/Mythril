#include "reload.shadergen.h"
#include "reload_edit.shadergen.h"
#include "reload_entry.shadergen.h"
#include "reload_layout.shadergen.h"
#include "reload_pointee.shadergen.h"
#include "reload_reorder.shadergen.h"
#include "reload_spec.shadergen.h"
#include "reload_spec_wide.shadergen.h"

#include "mythril/CTX.h"
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
	struct ReloadPass {
		mythril::ComputePipelineHandle pipeline;
		shaders::ReloadPush push;
	};

	std::vector<uint32_t> WithSecondEntryPoint(const std::filesystem::path& path) {
		std::vector<uint32_t> words(std::filesystem::file_size(path) / sizeof(uint32_t));
		std::ifstream(path, std::ios::binary).read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
		constexpr uint32_t kOpEntryPoint = 15;
		for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
			if ((words[i] & 0xffff) != kOpEntryPoint)
				continue;
			std::vector<uint32_t> copy(words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(i + (words[i] >> 16)));
			copy[3] ^= 1;
			words.insert(words.begin() + static_cast<std::ptrdiff_t>(i + copy.size()), copy.begin(), copy.end());
			break;
		}
		return words;
	}
} // namespace

#ifdef DEBUG
TEST_CASE("reloadShaderEXT takes a code edit and refuses one the C++ push structs or a pipeline cannot use") {
	mythril::CtxPtr ctx = mythril::CTXBuilder{}.set_vulkan_cfg({.app_name = "mythril_gpu_tests"}).build();
	REQUIRE(ctx);

	mythril::Buffer outBuffer = ctx->createBuffer({.size = sizeof(uint32_t), .usage = mythril::BufferUsageBits_Storage, .debugName = "Reload Out"});
	const std::filesystem::path spirvDir = MYTH_TEST_SPIRV_DIR;
	mythril::Shader shader = ctx->createShader({.path = spirvDir / shaders::reload::kSpirvFile, .debugName = "Reload Shader"});
	mythril::ComputePipeline pipeline = ctx->createComputePipeline({.shader = shader.handle(), .debugName = "Reload Pipeline"});

	const auto run = [&] {
		constexpr uint32_t kCleared = 0;
		ctx->uploadEXT(outBuffer.handle(), &kCleared, sizeof(kCleared));
		mythril::RenderGraph graph;
		const mythril::GraphBuffer rgOut = graph.importBuffer(outBuffer);
		graph.addComputePass<ReloadPass>("reload", [&](mythril::ComputePassBuilder& b, ReloadPass& d) {
			b.dependency(rgOut, mythril::BufferAccess::ShaderWrite);
			b.sideEffect();
			d = {pipeline.handle(), {.out = ctx->gpuAddress(outBuffer), .value = 7, .pad = 0}};
		}, [](const ReloadPass& d, mythril::ComputeCmd& cmd) {
			cmd.cmdBindComputePipeline(d.pipeline);
			cmd.cmdPushConstants(d.push);
			cmd.cmdDispatchThreadGroup({1, 1, 1});
		});
		graph.compile(*ctx);
		graph.execute(*ctx);
		ctx->waitIdle();
		std::vector<uint32_t> got(1);
		ctx->download(outBuffer.handle(), got);
		return got[0];
	};

	const auto reload = [&](const char* file) { return ctx->reloadShaderEXT(shader, spirvDir / file); };
	const auto writeFile = [](const std::filesystem::path& path, const std::vector<uint32_t>& words) {
		std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
	};

	REQUIRE(run() == 7);
	SUBCASE("an edited shader runs after the reload and the original after reloading it back") {
		CHECK(reload(shaders::reload_edit::kSpirvFile));
		CHECK(run() == 107);
		CHECK(reload(shaders::reload::kSpirvFile));
		CHECK(run() == 7);
	}
	SUBCASE("a shader whose entry point a pipeline names is gone is refused and the running code stays") {
		CHECK_FALSE(reload(shaders::reload_entry::kSpirvFile));
		CHECK(run() == 7);
		CHECK(reload(shaders::reload_edit::kSpirvFile));
		CHECK(run() == 107);
	}
	SUBCASE("a push layout with new members is refused and the running code stays") {
		CHECK_FALSE(reload(shaders::reload_layout::kSpirvFile));
		CHECK(run() == 7);
	}
	SUBCASE("reordered push members are refused and the running code stays") {
		CHECK_FALSE(reload(shaders::reload_reorder::kSpirvFile));
		CHECK(run() == 7);
	}
	SUBCASE("a change only behind a pointer is refused and the running code stays") {
		CHECK_FALSE(reload(shaders::reload_pointee::kSpirvFile));
		CHECK(run() == 7);
	}
	SUBCASE("files that are not usable SPIR-V are refused and the running code stays") {
		CHECK(reload(shaders::reload_edit::kSpirvFile));
		const std::filesystem::path garbage = spirvDir / "reload_garbage.spv";
		writeFile(garbage, std::vector<uint32_t>(64, 0xdeadbeef));
		CHECK_FALSE(ctx->reloadShaderEXT(shader, garbage));
		std::filesystem::remove(garbage);
		CHECK_FALSE(reload("reload_missing.spv"));
		const std::filesystem::path rules = spirvDir / "reload_rules.spv";
		writeFile(rules, WithSecondEntryPoint(spirvDir / shaders::reload::kSpirvFile));
		CHECK_FALSE(ctx->reloadShaderEXT(shader, rules));
		std::filesystem::remove(rules);
		CHECK(run() == 107);
	}
}

TEST_CASE("a shader whose specialization constant size a pipeline does not give is refused") {
	mythril::CtxPtr ctx = mythril::CTXBuilder{}.set_vulkan_cfg({.app_name = "mythril_gpu_tests", .additionalDeviceFeatures = {.shaderInt64 = VK_TRUE}}).build();
	REQUIRE(ctx);

	mythril::Buffer outBuffer = ctx->createBuffer({.size = sizeof(uint32_t), .usage = mythril::BufferUsageBits_Storage, .debugName = "Spec Out"});
	const std::filesystem::path spirvDir = MYTH_TEST_SPIRV_DIR;
	mythril::Shader shader = ctx->createShader({.path = spirvDir / shaders::reload_spec::kSpirvFile, .debugName = "Spec Shader"});
	const uint32_t add = 5;
	const uint64_t wideAdd = 5;
	mythril::ComputePipeline byName = ctx->createComputePipeline({.shader = shader.handle(), .specConstants = {{&add, sizeof(add), "kAdd"}}, .debugName = "Spec By Name"});
	mythril::ComputePipeline byId = ctx->createComputePipeline({.shader = shader.handle(), .specConstants = {{&add, sizeof(add), 3}}, .debugName = "Spec By Id"});
	// Vulkan ignores an id the shader does not have
	mythril::ComputePipeline unknownId = ctx->createComputePipeline({.shader = shader.handle(), .specConstants = {{&wideAdd, sizeof(wideAdd), 9}}, .debugName = "Spec Unknown Id"});

	const auto run = [&](const mythril::ComputePipeline& pipeline) {
		constexpr uint32_t kCleared = 0;
		ctx->uploadEXT(outBuffer.handle(), &kCleared, sizeof(kCleared));
		mythril::RenderGraph graph;
		const mythril::GraphBuffer rgOut = graph.importBuffer(outBuffer);
		graph.addComputePass<ReloadPass>("spec", [&](mythril::ComputePassBuilder& b, ReloadPass& d) {
			b.dependency(rgOut, mythril::BufferAccess::ShaderWrite);
			b.sideEffect();
			d = {pipeline.handle(), {.out = ctx->gpuAddress(outBuffer), .value = 7, .pad = 0}};
		}, [](const ReloadPass& d, mythril::ComputeCmd& cmd) {
			cmd.cmdBindComputePipeline(d.pipeline);
			cmd.cmdPushConstants(d.push);
			cmd.cmdDispatchThreadGroup({1, 1, 1});
		});
		graph.compile(*ctx);
		graph.execute(*ctx);
		ctx->waitIdle();
		std::vector<uint32_t> got(1);
		ctx->download(outBuffer.handle(), got);
		return got[0];
	};

	CHECK(run(byName) == 12);
	CHECK(run(byId) == 12);
	CHECK(run(unknownId) == 7);
	CHECK_FALSE(ctx->reloadShaderEXT(shader, spirvDir / shaders::reload_spec_wide::kSpirvFile));
	CHECK(run(byName) == 12);
	CHECK(run(byId) == 12);
}
#endif
