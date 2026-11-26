#include "roundtrip.shadergen.h"

#include "mythril/CTX.h"
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <vector>

namespace {
	struct Values {
		float next = 1.0f;
		std::vector<uint32_t> expected;

		float take() {
			expected.push_back(std::bit_cast<uint32_t>(next));
			return next++;
		}
		uint32_t take(const uint32_t value) {
			expected.push_back(value);
			return value;
		}
		template<class Matrix>
		Matrix matrix(const int rows, const int columns) {
			Matrix m{};
			for (int r = 0; r < rows; ++r)
				for (int c = 0; c < columns; ++c)
					m.m[r * columns + c] = take();
			return m;
		}
		mythril::shader::float2 vec2() { return {take(), take()}; }
		mythril::shader::float3 vec3() { return {take(), take(), take()}; }
		mythril::shader::float4 vec4() { return {take(), take(), take(), take()}; }
		shaders::Inner inner() { return {vec3(), take()}; }
	};

	struct RoundTripPass {
		mythril::ComputePipelineHandle pipeline;
		shaders::RoundTripPush push;
	};
} // namespace

TEST_CASE("every push block and pointee field reaches the GPU where the generated struct put it") {
	mythril::CtxPtr ctx = mythril::CTXBuilder{}.set_vulkan_cfg({.app_name = "mythril_gpu_tests"}).build();
	REQUIRE(ctx);

	Values values;
	shaders::RoundTripPush push{};
	push.m44 = values.matrix<mythril::shader::float4x4>(4, 4);
	push.m34 = values.matrix<mythril::shader::float3x4>(3, 4);
	push.v4 = values.vec4();
	push.v3 = values.vec3();
	push.i = static_cast<int32_t>(values.take(static_cast<uint32_t>(-7)));
	push.v2 = values.vec2();
	push.u2 = {values.take(0xdeadbeef), values.take(0x01234567)};
	push.inner = values.inner();
	push.arr[0] = values.vec3();
	push.arr[1] = values.vec3();
	shaders::Pointee pointee{};
	pointee.m44 = values.matrix<mythril::shader::float4x4>(4, 4);
	pointee.m34 = values.matrix<mythril::shader::float3x4>(3, 4);
	pointee.m43 = values.matrix<mythril::shader::float4x3>(4, 3);
	pointee.v3 = values.vec3();
	pointee.u = values.take(0xcafef00d);
	pointee.inner = values.inner();

	mythril::Buffer pointeeBuffer = ctx->createBuffer({.usage = mythril::BufferUsageBits_Storage, .initialData = pointee, .debugName = "Round Trip Pointee"});
	mythril::Buffer outBuffer = ctx->createBuffer({.size = values.expected.size() * sizeof(uint32_t), .usage = mythril::BufferUsageBits_Storage, .debugName = "Round Trip Out"});
	push.out = ctx->gpuAddress(outBuffer);
	push.pointee = ctx->gpuAddress(pointeeBuffer);

	mythril::Shader shader = ctx->createShader({.path = std::filesystem::path(MYTH_TEST_SPIRV_DIR) / shaders::roundtrip::kSpirvFile, .debugName = "Round Trip Shader"});
	mythril::ComputePipeline pipeline = ctx->createComputePipeline({.shader = shader.handle(), .debugName = "Round Trip Pipeline"});

	mythril::RenderGraph graph;
	const mythril::GraphBuffer rgPointee = graph.importBuffer(pointeeBuffer);
	const mythril::GraphBuffer rgOut = graph.importBuffer(outBuffer);
	graph.addComputePass<RoundTripPass>("round trip", [&](mythril::ComputePassBuilder& b, RoundTripPass& d) {
		b.dependency(rgPointee, mythril::BufferAccess::ShaderRead);
		b.dependency(rgOut, mythril::BufferAccess::ShaderWrite);
		b.sideEffect();
		d = {pipeline.handle(), push}; }, [](const RoundTripPass& d, mythril::ComputeCmd& cmd) {
		cmd.cmdBindComputePipeline(d.pipeline);
		cmd.cmdPushConstants(d.push);
		cmd.cmdDispatchThreadGroup({1, 1, 1}); });
	graph.compile(*ctx);
	graph.execute(*ctx);
	ctx->waitIdle();

	std::vector<uint32_t> got(values.expected.size());
	ctx->download(outBuffer.handle(), got);
	REQUIRE(got.size() == 16 + 12 + 4 + 3 + 1 + 2 + 2 + 4 + 6 + 16 + 12 + 12 + 3 + 1 + 4);
	for (size_t i = 0; i < got.size(); ++i) {
		CAPTURE(i);
		CHECK(got[i] == values.expected[i]);
	}
}
