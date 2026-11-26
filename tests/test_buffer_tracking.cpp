#include "RenderGraphInternal.h"
#include "sync_spec.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace mythril;
using namespace sync_spec;

namespace {
	constexpr vkutil::BufferSyncFeatures kAllFeatures = {.accelerationStructure = true, .rayTracingPipeline = true};

	std::vector<StageAccess> everyRequest() {
		constexpr BufferAccess kAccesses[] = {BufferAccess::ShaderRead, BufferAccess::ShaderWrite, BufferAccess::ShaderReadWrite, BufferAccess::IndexRead,
		                                      BufferAccess::IndirectRead, BufferAccess::TransferRead, BufferAccess::TransferWrite, BufferAccess::AccelerationStructureRead,
		                                      BufferAccess::AccelerationStructureBuildInput, BufferAccess::AccelerationStructureBuildRead, BufferAccess::AccelerationStructureBuildWrite,
		                                      BufferAccess::AccelerationStructureScratch};
		constexpr PassDesc::Type kPassTypes[] = {PassDesc::Type::Graphics, PassDesc::Type::Compute, PassDesc::Type::Transfer, PassDesc::Type::RayTracing};
		std::vector<StageAccess> out;
		for (BufferAccess access : kAccesses)
			for (PassDesc::Type type : kPassTypes) {
				const StageAccess a = vkutil::GetBufferStageAccess(access, type);
				bool seen = false;
				for (const StageAccess& b : out)
					seen |= b.stage == a.stage && b.access == a.access;
				if (!seen)
					out.push_back(a);
			}
		return out;
	}

	std::vector<std::string> check(const std::vector<StageAccess>& seq) {
		vkutil::BufferAccessTracker tracker = {};
		std::vector<Step> steps;
		for (size_t k = 0; k < seq.size(); ++k) {
			Step& step = steps.emplace_back(Step{"access " + std::to_string(k), {}, {seq[k]}});
			if (const std::optional<vkutil::BufferBarrier> barrier = vkutil::TrackBufferAccess(tracker, seq[k]))
				step.barriers.push_back(*barrier);
		}
		return Violations(steps, kAllFeatures);
	}

	std::vector<std::string> checkPass(const StageAccess prior, const std::vector<StageAccess>& uses) {
		vkutil::BufferAccessTracker tracker = {};
		vkutil::TrackBufferAccess(tracker, prior);
		std::vector<CompiledBufferBarrier> planned;
		for (const StageAccess& use : uses)
			AddBufferBarrier(planned, {}, use);
		Step pass{"pass", {}, uses};
		for (const CompiledBufferBarrier& request : planned)
			if (const std::optional<vkutil::BufferBarrier> barrier = vkutil::TrackBufferAccess(tracker, request.dstMask))
				pass.barriers.push_back(*barrier);
		const Step steps[] = {{"earlier pass", {}, {prior}}, pass};
		return Violations(steps, kAllFeatures);
	}
} // namespace

TEST_CASE("render graph buffer tracker orders every hazard for every request sequence up to length 4") {
	const std::vector<StageAccess> requests = everyRequest();
	const size_t n = requests.size();
	size_t checked = 0;
	std::vector<StageAccess> seq;
	const auto visit = [&](auto&& self, size_t depth) -> void {
		if (!seq.empty()) {
			++checked;
			for (const std::string& failure : check(seq))
				FAIL_CHECK("sequence of " << seq.size() << " starting with stage " << seq[0].stage << ": " << failure);
		}
		if (depth == 4)
			return;
		for (const StageAccess& a : requests) {
			seq.push_back(a);
			self(self, depth + 1);
			seq.pop_back();
		}
	};
	visit(visit, 0);
	CHECK(checked == n + n * n + n * n * n + n * n * n * n);
}

TEST_CASE("a buffer declared up to 3 times in one pass is ordered after every earlier access") {
	constexpr BufferAccess kAccesses[] = {BufferAccess::ShaderRead, BufferAccess::ShaderWrite, BufferAccess::ShaderReadWrite, BufferAccess::IndexRead,
	                                      BufferAccess::IndirectRead, BufferAccess::TransferRead, BufferAccess::TransferWrite, BufferAccess::AccelerationStructureRead,
	                                      BufferAccess::AccelerationStructureBuildInput, BufferAccess::AccelerationStructureBuildRead, BufferAccess::AccelerationStructureBuildWrite,
	                                      BufferAccess::AccelerationStructureScratch};
	constexpr PassDesc::Type kPassTypes[] = {PassDesc::Type::Graphics, PassDesc::Type::Compute, PassDesc::Type::Transfer, PassDesc::Type::RayTracing};
	const std::vector<StageAccess> priors = everyRequest();
	size_t checked = 0;
	for (const StageAccess& prior : priors)
		for (const PassDesc::Type type : kPassTypes) {
			std::vector<StageAccess> uses;
			const auto visit = [&](auto&& self) -> void {
				if (!uses.empty()) {
					++checked;
					for (const std::string& failure : checkPass(prior, uses))
						FAIL_CHECK("earlier stage " << prior.stage << ", " << uses.size() << " declarations, first stage " << uses[0].stage << ": " << failure);
				}
				if (uses.size() == 3)
					return;
				for (const BufferAccess access : kAccesses) {
					uses.push_back(vkutil::GetBufferStageAccess(access, type));
					self(self);
					uses.pop_back();
				}
			};
			visit(visit);
		}
	constexpr size_t n = std::size(kAccesses);
	CHECK(checked == priors.size() * std::size(kPassTypes) * (n + n * n + n * n * n));
}

TEST_CASE("acceleration structure accesses match what the spec says a build and a trace touch") {
	// spec, vkCmdBuildAccelerationStructuresKHR: dst and scratch are read and written, geometry and instance data is a shader read, every structure it reads is an acceleration structure read
	constexpr VkPipelineStageFlags2 kBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	constexpr VkAccessFlags2 kReadWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	const struct {
		BufferAccess access;
		StageAccess spec;
	} kBuildUses[] = {
	    {BufferAccess::AccelerationStructureBuildWrite, {kBuild, kReadWrite}},
	    {BufferAccess::AccelerationStructureScratch, {kBuild, kReadWrite}},
	    {BufferAccess::AccelerationStructureBuildInput, {kBuild, VK_ACCESS_2_SHADER_READ_BIT}},
	    {BufferAccess::AccelerationStructureBuildRead, {kBuild, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR}},
	};
	// spec, ray tracing and ray query: a shader reads the structure with an acceleration structure read in its own stage
	const struct {
		PassDesc::Type type;
		VkPipelineStageFlags2 stages;
	} kTracers[] = {
	    {PassDesc::Type::Graphics, VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT},
	    {PassDesc::Type::Compute, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
	    {PassDesc::Type::RayTracing, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR},
	};
	size_t checked = 0;
	for (const auto& use : kBuildUses) {
		const StageAccess got = vkutil::GetBufferStageAccess(use.access, PassDesc::Type::Transfer);
		CHECK(Stages(got.stage) == Stages(use.spec.stage));
		CHECK(Access(got.access) == Access(use.spec.access));
		CHECK(isWrite(use.access) == ((use.spec.access & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) != 0));
		++checked;
	}
	for (const auto& tracer : kTracers) {
		const StageAccess got = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureRead, tracer.type);
		CHECK(Stages(got.stage) == Stages(tracer.stages));
		CHECK(Access(got.access) == Access(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR));
		CHECK_FALSE(isWrite(BufferAccess::AccelerationStructureRead));
		++checked;
	}
	CHECK(checked == std::size(kBuildUses) + std::size(kTracers));
}

TEST_CASE("a structure build orders after the last trace and before the next one") {
	const StageAccess trace = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureRead, PassDesc::Type::RayTracing);
	const StageAccess build = vkutil::GetBufferStageAccess(BufferAccess::AccelerationStructureBuildWrite, PassDesc::Type::Transfer);
	CHECK(check({trace, build, trace, build}).empty());
	CHECK(isWrite(BufferAccess::AccelerationStructureBuildWrite));
	CHECK(isWrite(BufferAccess::AccelerationStructureScratch));
	CHECK_FALSE(isWrite(BufferAccess::AccelerationStructureBuildRead));
}
