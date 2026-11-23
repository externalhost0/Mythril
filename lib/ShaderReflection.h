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
#ifdef DEBUG
	void AnalyzePushAccess(const uint32_t* code, size_t size, std::vector<PushResourceField>& fields);
#endif
	inline PushResourceField::Access MergeAccess(const PushResourceField::Access a, const PushResourceField::Access b) {
		const auto merged = static_cast<uint8_t>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
		return merged & static_cast<uint8_t>(PushResourceField::Access::Unknown) ? PushResourceField::Access::Unknown : static_cast<PushResourceField::Access>(merged);
	}
} // namespace mythril
