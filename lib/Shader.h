//
// Created by Hayden Rivas on 11/1/25.
//

#pragma once

#include <volk.h>
#include <slang/slang.h>
#include <string>
#include <vector>

namespace mythril {
	enum class FieldKind {
		// special cases, can have their own kinds
		OpaqueHandle,
		Scalar,
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
		FieldKind fieldKind;

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
		std::string completeSlangName;
		VkShaderStageFlags usedStages;
		std::vector<FieldInfo> fields;
	};

	struct DescriptorSetSignature {
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		uint32_t setIndex;
		bool isBindless;
	};
	struct PipelineLayoutSignature {
		std::vector<DescriptorSetSignature> sets;
		std::vector<VkPushConstantRange> pushes;
	};

	PipelineLayoutSignature ReflectSPIRV(const uint32_t* code, size_t size);

	class Shader {
	public:
		struct ParameterBlockInfo : IParameterInfo {
			uint32_t setIndex;
			uint32_t bindingIndex;

			// for arrays (ie bindless), will usually be 1
			uint32_t descriptorCount;
			VkDescriptorType descriptorType;
		};
		struct PushConstantInfo : IParameterInfo {
			uint32_t offset;
			uint32_t size;
		};

		const std::vector<ParameterBlockInfo>& viewParameters() const { return _parameterBlocks; };
		const std::vector<PushConstantInfo>& viewPushConstants() const { return _pushConstants; }
		const PipelineLayoutSignature& getPipelineLayoutSignature() const { return _plSignature; }
		const std::string_view getnamefordebugpurpose() const { return _debugName; }
	private:
		// a shader defines not only the module obviously, but a pipelineLayout
		VkShaderModule vkShaderModule;

		// everything below can be quired by user, not necessary for renderer
		std::vector<ParameterBlockInfo> _parameterBlocks;
		std::vector<PushConstantInfo> _pushConstants;

		PipelineLayoutSignature _plSignature;
		bool usesBindlessSet = false;

		char _debugName[128] = {0};

		friend class CTX;
		friend class ShaderTransformer;
		friend class CommandBuffer;
	};


	class DescriptorSetLayoutBuilder {
	public:
		// descriptorCount is used for bindings that are arrays (ie: bindless descriptors)
		uint32_t getBindingCount() const;
		// always preffered that you give the stages but just in case user is lazy
		DescriptorSetLayoutBuilder& addBinding(uint32_t bindIndex, VkDescriptorType descriptorType, VkShaderStageFlags stages = VK_SHADER_STAGE_ALL, uint32_t descriptorCount = 1);

		VkDescriptorSetLayout build(VkDevice vkDevice);
	private:
		void clear();
		std::vector<VkDescriptorSetLayoutBinding> _vkDescriptorRanges;
		uint32_t setIndex = -1;

		friend class ShaderTransformer;
	};

	class PipelineLayoutBuilder {
	public:
		PipelineLayoutBuilder& addDescriptorSetLayoutBuilder(DescriptorSetLayoutBuilder& dsBuilder);
		PipelineLayoutBuilder& addPushConstantRange(VkPushConstantRange pcRange);

		VkPipelineLayout build(VkDevice device);
	private:
		void clear();
		std::vector<DescriptorSetLayoutBuilder> _dslBuilders;
		std::vector<VkPushConstantRange> _vkPCRs;

		// invalid until later
		std::vector<VkDescriptorSetLayout> _builtDSLs;

		friend class ShaderTransformer;
	};



	class ShaderTransformer {
	public:
		void performReflection(Shader& shader, slang::ShaderReflection* reflectionData);
	private:
		void addRangesForParameterBlockElement(PipelineLayoutSignature& plSignature,
											   DescriptorSetSignature& dslSignature,
											   slang::TypeLayoutReflection* elementTypeLayout);
		void addAutomaticallyIntroducedUniformBuffer(DescriptorSetSignature& dslSignature);
		void addDescriptorSetForParameterBlock(PipelineLayoutSignature& plSignature,
											   slang::TypeLayoutReflection* paramBlockTypeLayout);
		void addPushConstantRangeForConstantBuffer(PipelineLayoutSignature& plSignature,
												   slang::TypeLayoutReflection* pushConstantBufferTypeLayout);
		void addRanges(PipelineLayoutSignature& plSignature,
					   DescriptorSetSignature& dslSignature,
					   slang::TypeLayoutReflection* typeLayout);
		void addDescriptorRanges(DescriptorSetSignature& dslSignature,
								 slang::TypeLayoutReflection* typeLayout);
		void addDescriptorRange(DescriptorSetSignature& dslSignature,
								slang::TypeLayoutReflection* typeLayout,
								int relativeSetIndex,
								int rangeIndex);
		void addSubRanges(PipelineLayoutSignature& plSignature,
						  slang::TypeLayoutReflection* typeLayout);
		void addSubRange(PipelineLayoutSignature& plSignature,
						 slang::TypeLayoutReflection* typeLayout,
						 int subRangeIndex);
		void startBuildingDescriptorSetLayout(PipelineLayoutSignature& plSignature,
											  DescriptorSetSignature& dslSignature);
		void finishBuildingDescriptorSetLayout(PipelineLayoutSignature& plSignature,
											   DescriptorSetSignature& dslSignature);
//		void addRangesForParameterBlockElement(PipelineLayoutBuilder& plBuilder,
//											   DescriptorSetLayoutBuilder& dslBuilder,
//											   slang::TypeLayoutReflection* elementTypeLayout);
//
//		void addAutomaticallyIntroducedUniformBuffer(DescriptorSetLayoutBuilder &dslBuilder);
//
//		void addDescriptorSetForParameterBlock(PipelineLayoutBuilder& plBuilder,
//											   slang::TypeLayoutReflection* paramBlockTypeLayout);
//		void addPushConstantRangeForConstantBuffer(PipelineLayoutBuilder& plBuilder,
//												   slang::TypeLayoutReflection* pushConstantBufferTypeLayout);
//
//
//		void addRanges(PipelineLayoutBuilder& plBuilder,
//					   DescriptorSetLayoutBuilder& dslBuilder,
//					   slang::TypeLayoutReflection* typeLayout);
//
//		void addDescriptorRanges(DescriptorSetLayoutBuilder& dslBuilder,
//								 slang::TypeLayoutReflection* typeLayout);
//		void addDescriptorRange(DescriptorSetLayoutBuilder& dslBuilder,
//								slang::TypeLayoutReflection* typeLayout,
//								int relativeSetIndex,
//								int rangeIndex);
//
//		void addSubRanges(PipelineLayoutBuilder& plBuilder,
//						  slang::TypeLayoutReflection* typeLayout);
//		void addSubRange(PipelineLayoutBuilder& plBuilder,
//						 slang::TypeLayoutReflection* typeLayout,
//						 int subRangeIndex);
//
//		void startBuildingDescriptorSetLayout(PipelineLayoutBuilder& plBuilder,
//											  DescriptorSetLayoutBuilder& dslBuilder);
//		void finishBuildingDescriptorSetLayout(PipelineLayoutBuilder& plBuilder,
//											   DescriptorSetLayoutBuilder& dslBuilder);
	private:
		Shader* _currentShader = nullptr;
		VkShaderStageFlags _currentStageFlags = VK_SHADER_STAGE_ALL;
	};
}