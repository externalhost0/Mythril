#pragma once

#include "mythril/Shader.h"

#include <vector>

namespace mythril {
	struct ReflectionResult {
		// ds = descriptor sets
		// pc = push constants
		// sc = specialization constants

		// pipeline layout requires ds & pc
		PipelineLayoutSignature pipelineLayoutSignature;
		// pipeline requires sc
		SpecializationInfo specializationInfo;

		// optional information for user-created reflection systems
		// ie extra memory usage
		std::vector<detail::AllocatedShader::DescriptorSetInfo> retrievedDescriptorSets;
		std::vector<detail::AllocatedShader::PushConstantInfo> retrivedPushConstants;
	};

	void PatchSpecConstants(uint32_t*& code, size_t& size);
	ReflectionResult ReflectSPIRV(const uint32_t* code, size_t size);
} // namespace mythril
