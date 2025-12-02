//
// Created by Hayden Rivas on 11/1/25.
//

#pragma once

#include <volk.h>
#include <slang/slang.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace mythril {
	enum class FieldKind {
		// special cases, can have their own kinds
		OpaqueHandle,
		Scalar,
		Pointer,
		// ordinary cases
		Vector,
		Matrix,
		Array,
		Struct,

		Unknown
	};
	enum class OpaqueKind {
		// sampled images
		Texture1D,
		Texture2D,
		Texture3D,
		TextureCube,

		// samplers
		Sampler,

		// storage images
		RWTexture1D,
		RWTexture2D,
		RWTexture3D,

		Unknown
	};
	enum class ScalarKind {
		None,
		Int,
		UInt,
		Float,
		Double,
		Bool
	};

	struct FieldInfo {
		// always valid
		std::string varName;
		std::string typeName;
		uint32_t size;
		uint32_t offset;
		FieldKind kind;

		// only valid when FieldKind::Scalar
		ScalarKind scalarKind;
		// valid for FieldKind::OpaqueHandle
		OpaqueKind opaqueKind;

		// valid for vectors
		uint32_t componentCount = 0;
		// valid for matrices
		uint32_t rowCount = 0;
		uint32_t columnCount = 0;

		// valid for structs
		std::vector<FieldInfo> fields;
	};
	struct IParameterInfo {
		std::string varName;
		std::string typeName;
		VkShaderStageFlags usedStages;
		std::vector<FieldInfo> fields;
	};

	struct DescriptorSetSignature {
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		std::unordered_map<std::string, uint32_t> nameToBinding;
		uint32_t setIndex;
		bool isBindless;
	};

	struct PipelineLayoutSignature {
		std::vector<DescriptorSetSignature> setSignatures;
		std::vector<VkPushConstantRange> pushes;
	};


	class Shader {
	public:
		struct DescriptorBindingInfo : IParameterInfo {
			uint32_t setIndex;
			uint32_t bindingIndex;

			// for arrays (ie bindless), will usually be 1
			uint32_t descriptorCount;
			VkDescriptorType descriptorType;
		};
		// these in a vector should be ordered according to their set index anyway
		struct DescriptorSetInfo {
			uint32_t setIndex;
			std::vector<DescriptorBindingInfo> bindingInfos;
		};
		struct PushConstantInfo : IParameterInfo {
			uint32_t offset;
			uint32_t size;
		};

		const std::vector<DescriptorSetInfo>& viewDescriptorSets() const { return _descriptorSets; };
		const std::vector<PushConstantInfo>& viewPushConstants() const { return _pushConstants; }
		const PipelineLayoutSignature& getPipelineLayoutSignature() const { return _pipelineSignature; }
		std::string_view getnamefordebugpurpose() const { return _debugName; }
	private:
		// a shader defines not only the module obviously, but a pipelineLayout
		VkShaderModule vkShaderModule;

		// everything below can be quired by user, not used by Mythril at all
		std::vector<DescriptorSetInfo> _descriptorSets;
		std::vector<PushConstantInfo> _pushConstants;

		PipelineLayoutSignature _pipelineSignature;

		char _debugName[128] = {0};

		friend class CTX;
		friend class CommandBuffer;
	};

	struct ReflectionResult {
		PipelineLayoutSignature pipelineLayoutSignature;
		// optional information for user-created reflection systems
		std::vector<Shader::DescriptorSetInfo> retrievedDescriptorSets;
		std::vector<Shader::PushConstantInfo> retrivedPushConstants;
	};

	ReflectionResult ReflectSPIRV(const uint32_t* code, size_t size);

}