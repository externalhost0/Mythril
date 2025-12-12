//
// Created by Hayden Rivas on 11/1/25.
//

#include "Shader.h"
#include "HelperMacros.h"
#include "Logger.h"

#include <string>
#include <optional>
#include <cctype>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>
#include <spirv_reflect.h>


namespace mythril {
	static const char* VulkanDescriptorTypeToString(VkDescriptorType type) {
		switch (type) {
			case VK_DESCRIPTOR_TYPE_SAMPLER:
				return "VK_DESCRIPTOR_TYPE_SAMPLER";
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER";
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE";
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE";
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER";
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				return "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER";
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER";
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER";
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC";
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC";
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				return "VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT";
			case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
				return "VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK";
			case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
				return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR";
			case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
				return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV";
			case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
				return "VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM";
			case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
				return "VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM";
			case VK_DESCRIPTOR_TYPE_TENSOR_ARM:
				return "VK_DESCRIPTOR_TYPE_TENSOR_ARM";
			case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
				return "VK_DESCRIPTOR_TYPE_MUTABLE_EXT";
			case VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV:
				return "VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV";
			default:
				ASSERT_MSG(false, "VkDescriptorType should have a value.");
		}
	}

	const char* FieldKindToString(FieldKind fieldKind) {
		switch (fieldKind) {
			case FieldKind::OpaqueHandle: return "OpaqueHandle";
			case FieldKind::Scalar:       return "Scalar";
			case FieldKind::Vector:       return "Vector";
			case FieldKind::Matrix:       return "Matrix";
			case FieldKind::Array:        return "Array";
			case FieldKind::Struct:       return "Struct";
			case FieldKind::Pointer:      return "Pointer";
			case FieldKind::Unknown:      return "Unknown";
		}
	}
	const char* ScalarKindToString(ScalarKind scalarKind) {
		switch (scalarKind) {
			case ScalarKind::None: return "None";
			case ScalarKind::Int: return "Int";
			case ScalarKind::UInt: return "UInt";
			case ScalarKind::Float: return "Float";
			case ScalarKind::Double: return "Double";
			case ScalarKind::Bool: return "Bool";
		}
	}

	struct MatrixDims {
		int rows;
		int cols;
	};

	static std::optional<MatrixDims> ExtractMatrixDims(const std::string& s) {
		// for length of string
		for (size_t i = 0; i < s.size(); i++) {
			// if character is digit, as this is the first part of detecting NxM pattern
			if (std::isdigit(s[i])) {
				// parse first number
				int rows = s[i] - '0';
				size_t j = i + 1;
				// allow multi-digit numbers
				while (j < s.size() && std::isdigit(s[j])) {
					rows = rows * 10 + (s[j] - '0');
					j++;
				}
				// expect 'x'
				if (j >= s.size() || s[j] != 'x')
					continue;
				j++;
				// parse second number
				if (j >= s.size() || !std::isdigit(s[j]))
					continue;
				int cols = s[j] - '0';
				j++;
				while (j < s.size() && std::isdigit(s[j])) {
					cols = cols * 10 + (s[j] - '0');
					j++;
				}
				return MatrixDims{ rows, cols };
			}
		}
		return std::nullopt;
	}

	static bool DetectMatrixType(const SpvReflectTypeDescription& typeDesc) {
		if (typeDesc.struct_type_description) {
			std::string s = typeDesc.type_name;
			return s.find("MatrixStorage") != std::string::npos;
		}
		return false;
	}

	static FieldKind ResolveKind(const SpvReflectTypeDescription& typeDesc) {
		// it looks very bad with all of the if statements but it makes sense alright
		if (DetectMatrixType(typeDesc)) {
			return FieldKind::Matrix;
		}
		if (typeDesc.struct_type_description) {
			return FieldKind::Struct;
		}
		if (typeDesc.op == SpvOpTypePointer) {
			return FieldKind::Pointer;
		}
		if (typeDesc.traits.numeric.vector.component_count > 0) {
			return FieldKind::Vector;
		}
		if (typeDesc.traits.array.dims_count > 0) {
			return FieldKind::Array;
		}
		if (typeDesc.traits.numeric.scalar.width > 0) {
			return FieldKind::Scalar;
		}
		return FieldKind::Unknown;
	}

	static ScalarKind ResolveScalarKindFromType(const SpvReflectTypeDescription& typeDesc) {
		switch (typeDesc.op) {
			case SpvOpTypeInt: {
				if (typeDesc.traits.numeric.scalar.signedness == 1) {
					return ScalarKind::Int;
				}
				return ScalarKind::UInt;
			}
			case SpvOpTypeFloat: {
				if (typeDesc.traits.numeric.scalar.width == 64) {
					return ScalarKind::Double;
				}
				return ScalarKind::Float;
			}
			case SpvOpTypeBool: return ScalarKind::Bool;
			default: break;
		}
		ASSERT_MSG(false, "Something went horribly wrong, this should never happen!");
	}

	static void CollectFieldsRecursively(std::vector<FieldInfo>& fields, const SpvReflectBlockVariable& blockVar) {
		for (unsigned int i = 0; i < blockVar.member_count; i++) {
			// aliases
			const SpvReflectBlockVariable& memberVar = blockVar.members[i];
			const SpvReflectTypeDescription& memberType = *memberVar.type_description;

			FieldInfo field;
			field.size = memberVar.size;
			field.offset = memberVar.offset;
			field.varName = memberVar.name;
			field.kind = ResolveKind(memberType);

			switch (field.kind) {
				case FieldKind::Scalar: {
					field.typeName = "Scalar";
					field.scalarKind = ResolveScalarKindFromType(memberType);
				} break;
				case FieldKind::Pointer: {
					field.typeName = "Pointer";
				} break;
				case FieldKind::Struct: {
					field.typeName = memberType.type_name;
					CollectFieldsRecursively(field.fields, memberVar);
				} break;
				case FieldKind::Array: {
					field.typeName = "Array";
					field.componentCount = memberVar.array.dims_count;
				} break;
				case FieldKind::Vector: {
					field.typeName = "Vector";
					field.componentCount = memberType.traits.numeric.vector.component_count;
				} break;
				case FieldKind::Matrix: {
					field.typeName = "Matrix";
					std::optional<MatrixDims> extracted_dims = ExtractMatrixDims(memberType.type_name);
					if (auto dims = extracted_dims) {
						field.rowCount = dims->rows;
						field.columnCount = dims->cols;
					}
					ASSERT_MSG(extracted_dims.has_value(), "Extracted dims is nullopt!");
				} break;
				default: {
					ASSERT_MSG(false, "This should never happen, kind could not be resolved!");
				}
			}
			fields.push_back(field);
		}
	}


	static Shader::DescriptorBindingInfo GatherDescriptorBindingInformation(const SpvReflectDescriptorBinding& descriptorBind) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *descriptorBind.type_description;
		Shader::DescriptorBindingInfo info = {};
		// descriptor info
		info.descriptorType = static_cast<VkDescriptorType>(descriptorBind.descriptor_type);
		info.descriptorCount = descriptorBind.count;
		info.setIndex = descriptorBind.set;
		info.bindingIndex = descriptorBind.binding;
		// top level info
		info.typeName = blockType.type_name;
		info.varName = descriptorBind.name;

		CollectFieldsRecursively(info.fields, descriptorBind.block);
		return info;
	}


	static Shader::PushConstantInfo GatherPushConstantInformation(const SpvReflectBlockVariable& blockVar) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *blockVar.type_description;
		Shader::PushConstantInfo info = {};
		// top level info
		info.size = blockVar.size;
		info.offset = blockVar.offset;
		info.varName = blockVar.name;
		info.typeName = blockType.type_name;

		CollectFieldsRecursively(info.fields, blockVar);
		return info;
	}


//	void collectFieldsRecursivelyEXT(std::vector<FieldInfo>& fields, slang::TypeLayoutReflection* typeLayout) {
//		unsigned int fieldCount = typeLayout->getFieldCount();
//		for (unsigned int i = 0; i < fieldCount; ++i) {
//			auto fieldVariableLayout = typeLayout->getFieldByIndex(i);
//			auto fieldTypeLayout = fieldVariableLayout->getTypeLayout();
//
//			FieldInfo field;
//			field.varName = fieldVariableLayout->getName();
//			field.typeName = fieldTypeLayout->getName();
//			field.kind = getFieldKind(fieldTypeLayout);
//			field.scalarKind = getScalarType(fieldTypeLayout);
//			field.size = fieldTypeLayout->getSize();
//			field.offset = fieldVariableLayout->getOffset();
//
//			// must be take from variableReflection
//			// TODO: remove me before merge
//			if (const char* string = fieldVariableLayout->getVariable()->getUserAttributeByIndex(0)->getName()) {
//				int hasMark = strcmp(string, "MarkHandle");
//				if (hasMark == 0) {
//					int val;
//					fieldVariableLayout->getVariable()->getUserAttributeByIndex(0)->getArgumentValueInt(0, &val);
//					field.opaqueKind = (OpaqueKind)val;
//					field.kind = FieldKind::OpaqueHandle;
//					field.scalarKind = ScalarKind::None;
//					field.typeName = "OpaqueHandle";
//				}
//			}
//
//			auto type = fieldTypeLayout->getType();
//
//			// special cases of fields that need more data
//			switch (field.kind) {
//				case FieldKind::Vector: {
//					field.componentCount = type->getElementCount();
//				} break;
//				case FieldKind::Matrix: {
//					field.rowCount = type->getRowCount();
//					field.columnCount = type->getColumnCount();
//				} break;
//				case FieldKind::Array: {
//					field.componentCount = type->getElementCount();  // Array size
//					// Recursively collect the array element type
//					auto elementTypeLayout = fieldTypeLayout->getElementTypeLayout();
//					if (elementTypeLayout && elementTypeLayout->getFieldCount() > 0) {
//						collectFieldsRecursivelyEXT(field.fields, elementTypeLayout);
//					}
//				} break;
//				case FieldKind::Struct: {
//					collectFieldsRecursivelyEXT(field.fields, fieldTypeLayout);
//				} break;
//				default:
//					LOG_DEBUG("Unaccounted field of kind '{}', type '{}', named '{}'", FieldKindToString(field.kind), field.typeName, field.varName);
//			}
//			fields.push_back(field);
//		}
//	}


	ReflectionResult ReflectSPIRV(const uint32_t* code, size_t size) {
		SpvReflectShaderModule spirv_module = {};
		SpvReflectResult spv_result = spvReflectCreateShaderModule(size, code, &spirv_module);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Initial reflection of the SpvReflectShaderModule failed!");

		VkShaderStageFlags vkShaderStageFlags = 0;
		uint32_t entry_point_count = spirv_module.entry_point_count;
		for (int i_entryPoint = 0; i_entryPoint < entry_point_count; i_entryPoint++) {
			SpvReflectEntryPoint reflectedEntryPoint = spirv_module.entry_points[i_entryPoint];
			vkShaderStageFlags |= reflectedEntryPoint.shader_stage;
		}

		// define return value
		ReflectionResult result = {};

		// collect specialization constants
		uint32_t sc_count = 0;
		spv_result = spvReflectEnumerateSpecializationConstants(&spirv_module, &sc_count, nullptr);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Specialization Constants for count.");

		std::vector<SpvReflectSpecializationConstant*> constants(sc_count);
		spv_result = spvReflectEnumerateSpecializationConstants(&spirv_module, &sc_count, constants.data());
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Specialization Constants for data.");

		for (const SpvReflectSpecializationConstant* reflected_constant : constants) {
			// safely derefrence
			const SpvReflectTypeDescription& constantType = *reflected_constant->type_description;
			SpecializationConstant info = {};

			info.varName = reflected_constant->name;
			info.typeName = "Scalar";
			info.scalarKind = ResolveScalarKindFromType(constantType);
			info.id = reflected_constant->constant_id;

			result.specializationInfo.specializationConstants.emplace_back(info);
			result.specializationInfo.nameToID.insert({info.varName, info.id});
		}

		// collect descriptor sets info

		uint32_t ds_count = 0;
		spv_result = spvReflectEnumerateDescriptorSets(&spirv_module, &ds_count, nullptr);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for count.");

		std::vector<SpvReflectDescriptorSet*> sets(ds_count);
		spv_result = spvReflectEnumerateDescriptorSets(&spirv_module, &ds_count, sets.data());
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for data.");

		// begin collecting information for the result here
		result.pipelineLayoutSignature.setSignatures.resize(sets.size());
		result.retrievedDescriptorSets.reserve(sets.size()); // this reserves +1 when bindless exists

		// lets make every shader own its descriptor sets even if they are duplicated, optimize later with a pool of a sort whe signatures match
		for (int set_index = 0; set_index < sets.size(); set_index++) {
			// get a set from the array we enumerated
			const SpvReflectDescriptorSet* reflected_set = sets[set_index];
			// modify the set signature in place
			DescriptorSetSignature& set_signature = result.pipelineLayoutSignature.setSignatures[set_index];
			set_signature.isBindless = false;
			set_signature.setIndex = reflected_set->set;
			set_signature.bindings.reserve(reflected_set->binding_count);

			// for bonus reflection, as done in push constants aswell
			// filled in later by each binding, must be done as we move across bindings in order to avoid bindless descriptor
			Shader::DescriptorSetInfo set_info = {};
			set_info.setIndex = reflected_set->set;

			// now move through all the descriptor bindings in the set
			for (int binding_index = 0; binding_index < reflected_set->binding_count; binding_index++) {
				// get a binding from the set
				const SpvReflectDescriptorBinding* reflected_binding = reflected_set->bindings[binding_index];
				auto vk_descriptor_type = static_cast<VkDescriptorType>(reflected_binding->descriptor_type);

				// resolve custom bindless status
				if (strcmp(reflected_binding->name, "__slang_resource_heap") == 0) {
					set_signature.isBindless = true;
					continue;
				}
				ASSERT_MSG(
						vk_descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
						vk_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						"VkDescriptorType '{}' not currently supported!", VulkanDescriptorTypeToString(vk_descriptor_type));

				// fill in the vk struct
				VkDescriptorSetLayoutBinding vkds_layout_binding = {};
				vkds_layout_binding.binding = reflected_binding->binding;
				vkds_layout_binding.descriptorType = static_cast<VkDescriptorType>(reflected_binding->descriptor_type);
				vkds_layout_binding.descriptorCount = reflected_binding->count;
				vkds_layout_binding.stageFlags = vkShaderStageFlags;
				vkds_layout_binding.pImmutableSamplers = nullptr;
				// thats all the info for a binding
				// this is for lookup when updating
				set_signature.nameToBinding.insert({reflected_binding->name, reflected_binding->binding});

				// bonus information for user
				set_info.bindingInfos.emplace_back(GatherDescriptorBindingInformation(*reflected_binding));

				set_signature.bindings.push_back(vkds_layout_binding);
			}
			if (!set_info.bindingInfos.empty()) result.retrievedDescriptorSets.push_back(set_info);
		}

		// collect push constant info
		uint32_t pc_count = 0;
		spv_result = spvReflectEnumeratePushConstantBlocks(&spirv_module, &pc_count, nullptr);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate PushConstant Blocks for count.");

		std::vector<SpvReflectBlockVariable*> blocks(pc_count);
		spv_result = spvReflectEnumeratePushConstantBlocks(&spirv_module, &pc_count, blocks.data());
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate PushConstant Blocks for count.");

		std::vector<VkPushConstantRange> pc_ranges(blocks.size());
		result.retrivedPushConstants.reserve(blocks.size());
		for (int i_block = 0; i_block < blocks.size(); i_block++) {
			const SpvReflectBlockVariable* block = blocks[i_block];

			VkPushConstantRange pc_range = {};
			pc_range.size = block->size;
			pc_range.offset = block->offset;
			pc_range.stageFlags = vkShaderStageFlags;
			pc_ranges[i_block] = pc_range;

			result.retrivedPushConstants.emplace_back(GatherPushConstantInformation(*block));
		}
		result.pipelineLayoutSignature.pushes = std::move(pc_ranges);

		spvReflectDestroyShaderModule(&spirv_module);

		return result;
	}

//	void ShaderTransformer::startBuildingDescriptorSetLayout(PipelineLayoutSignature& plSignature, DescriptorSetSignature& dslSignature) {
//		dslSignature.setIndex = plSignature.setSignatures.size() + 1;
//	}
//
//	void ShaderTransformer::finishBuildingDescriptorSetLayout(PipelineLayoutSignature& plSignature, DescriptorSetSignature& dslSignature) {
//		if (dslSignature.bindings.empty())
//			return;
//		ASSERT_MSG(dslSignature.setIndex == plSignature.setSignatures.size() + 1, "Set index doesnt align with descriptor sets vector size!");
//		plSignature.setSignatures.push_back(std::move(dslSignature));
//	}
//
//	void ShaderTransformer::performReflection(Shader& shader, slang::ProgramLayout* programLayout) {
//		printf(" ----- Entering Shader: %s ----- \n", shader._debugName);
//		_currentShader = &shader;
//
//		// Clear the signature
//		shader._pipelineSignature.setSignatures.clear();
//		shader._pipelineSignature.pushes.clear();
//
//		DescriptorSetSignature dslSignature;
//		startBuildingDescriptorSetLayout(shader._pipelineSignature, dslSignature);
//
//		// GLOBAL PARAMETERS
//		_currentStageFlags = VK_SHADER_STAGE_ALL;
//		printf(" -- Entering Global Params -- \n");
//		addRangesForParameterBlockElement(shader._pipelineSignature, dslSignature, programLayout->getGlobalParamsTypeLayout());
//
//		// ENTRY POINT PARAMETERS
////		printf(" -- Entering Entry Points -- \n");
////		auto entryPointCount = static_cast<int>(programLayout->getEntryPointCount());
////		for (int i = 0; i < entryPointCount; i++) {
////			auto entryPointLayout = programLayout->getEntryPointByIndex(i);
////			_currentStageFlags = SlangStageToVkShaderStage(entryPointLayout->getStage());
////			addRangesForParameterBlockElement(shader._plSignature, dslSignature, entryPointLayout->getTypeLayout());
////		}
//
//		finishBuildingDescriptorSetLayout(shader._pipelineSignature, dslSignature);
//		printf("\n");
//	}
//
//	void ShaderTransformer::addRangesForParameterBlockElement(PipelineLayoutSignature& plSignature,
//															  DescriptorSetSignature& dslSignature,
//															  slang::TypeLayoutReflection* elementTypeLayout) {
//		if (elementTypeLayout->getSize() > 0) {
//			addAutomaticallyIntroducedUniformBuffer(dslSignature);
//		}
//		addRanges(plSignature, dslSignature, elementTypeLayout);
//	}
//
//	void ShaderTransformer::addAutomaticallyIntroducedUniformBuffer(DescriptorSetSignature& dslSignature) {
//		auto vulkanBindingIndex = dslSignature.bindings.size();
//		VkDescriptorSetLayoutBinding binding = {
//				.binding = static_cast<uint32_t>(vulkanBindingIndex),
//				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
//				.descriptorCount = 1,
//				.stageFlags = _currentStageFlags,
//				.pImmutableSamplers = nullptr,
//		};
//		dslSignature.bindings.push_back(binding);
//	}
//
//	void ShaderTransformer::addRanges(PipelineLayoutSignature& plSignature,
//									  DescriptorSetSignature& dslSignature,
//									  slang::TypeLayoutReflection* typeLayout) {
//		printf("Adding Both Ranges - %s\n", typeLayout->getName());
//		addDescriptorRanges(dslSignature, typeLayout);
//		addSubRanges(plSignature, typeLayout);
//	}
//
//	void ShaderTransformer::addDescriptorRanges(DescriptorSetSignature& dslSignature,
//												slang::TypeLayoutReflection* typeLayout) {
//		int relativeSetIndex = 0;
//		auto rangeCount = static_cast<unsigned int>(typeLayout->getDescriptorSetDescriptorRangeCount(relativeSetIndex));
//		printf("Adding Descriptor Range for %s with a count of %d\n", typeLayout->getName(), rangeCount);
//		for (int rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
//			addDescriptorRange(dslSignature, typeLayout, relativeSetIndex, rangeIndex);
//		}
//	}
//
//	void ShaderTransformer::addDescriptorRange(DescriptorSetSignature& dslSignature,
//											   slang::TypeLayoutReflection* typeLayout,
//											   int relativeSetIndex,
//											   int rangeIndex) {
//		slang::BindingType bindingType = typeLayout->getDescriptorSetDescriptorRangeType(relativeSetIndex, rangeIndex);
//		const uint32_t descriptorCount = typeLayout->getDescriptorSetDescriptorRangeDescriptorCount(relativeSetIndex, rangeIndex);
//
//		// avoid push constants, they cant fit in descriptors
//		if (bindingType == slang::BindingType::PushConstant) return;
//
//		auto bindingIndex = dslSignature.bindings.size();
//		VkDescriptorSetLayoutBinding binding = {
//				.binding = static_cast<uint32_t>(bindingIndex),
//				.descriptorType = mapToDescriptorType(bindingType),
//				.descriptorCount = descriptorCount,
//				.stageFlags = _currentStageFlags,
//				.pImmutableSamplers = nullptr,
//		};
//		dslSignature.bindings.push_back(binding);
//	}
//
//	void ShaderTransformer::addSubRanges(PipelineLayoutSignature& plSignature,
//										 slang::TypeLayoutReflection* typeLayout) {
//		auto subRangeCount = static_cast<unsigned int>(typeLayout->getSubObjectRangeCount());
//		printf("SubRange Count: %d\n", subRangeCount);
//		for (int subRangeIndex = 0; subRangeIndex < subRangeCount; ++subRangeIndex) {
//			addSubRange(plSignature, typeLayout, subRangeIndex);
//		}
//	}
//
//	void ShaderTransformer::addSubRange(PipelineLayoutSignature& plSignature,
//										slang::TypeLayoutReflection* typeLayout,
//										int subRangeIndex) {
//		auto bindingRangeIndex = static_cast<unsigned int>(typeLayout->getSubObjectRangeBindingRangeIndex(subRangeIndex));
//		slang::BindingType bindingType = typeLayout->getBindingRangeType(bindingRangeIndex);
//
//		switch (bindingType) {
//			default:
//				ASSERT_MSG(false, "Invalid BindingTypes Detected: '{}' is not allowed, you may only have ParameterBlocks and Push Constants at top level!", BindingTypeToString(bindingType));
//			case slang::BindingType::ParameterBlock: {
//				auto parameterBlockTypeLayout = typeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);
//				auto parameterBlockVariableLayout = typeLayout->getFieldByIndex(bindingRangeIndex);
//
//				auto elementTypeLayout = parameterBlockTypeLayout->getElementTypeLayout();
//
//				Shader::DescriptorBindingInfo bindingInfo;
//				bindingInfo.varName = parameterBlockVariableLayout->getName();
//				bindingInfo.typeName = elementTypeLayout->getName();
//
//				Slang::ComPtr<slang::IBlob> nameBlob;
//				parameterBlockTypeLayout->getType()->getFullName(nameBlob.writeRef());
//				bindingInfo.completeSlangName = static_cast<const char*>(nameBlob->getBufferPointer());
//
//				bindingInfo.descriptorCount = 1;
//				bindingInfo.usedStages = _currentStageFlags;
//				bindingInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
//
//				collectFieldsRecursivelyEXT(bindingInfo.fields, elementTypeLayout);
//
//				addDescriptorSetForParameterBlock(plSignature, parameterBlockTypeLayout);
//
//				// get the binding index from the just added descriptor set
//				const DescriptorSetSignature& lastSet = plSignature.setSignatures.back();
//				unsigned int vulkanBindingIndex = lastSet.bindings.size() - 1;
//				bindingInfo.bindingIndex = vulkanBindingIndex;
//				bindingInfo.setIndex = parameterBlockVariableLayout->getOffset(slang::ParameterCategory::SubElementRegisterSpace);
//
//				_currentShader->_parameterBlocks.push_back(bindingInfo);
//			} break;
//
//			case slang::BindingType::PushConstant: {
//				auto constantBufferTypeLayout = typeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);
//				auto constantBufferVariableLayout = typeLayout->getFieldByIndex(bindingRangeIndex);
//
//				auto elementTypeLayout = constantBufferTypeLayout->getElementTypeLayout();
//
//				Shader::PushConstantInfo info;
//				info.varName = constantBufferVariableLayout->getName();
//				info.typeName = constantBufferTypeLayout->getName();
//				Slang::ComPtr<slang::IBlob> nameBlob;
//				constantBufferTypeLayout->getType()->getFullName(nameBlob.writeRef());
//				info.completeSlangName = static_cast<const char*>(nameBlob->getBufferPointer());
//				info.usedStages = _currentStageFlags;
//				info.offset = 0;
//				info.size = constantBufferTypeLayout->getElementTypeLayout()->getSize();
//
//				collectFieldsRecursivelyEXT(info.fields, elementTypeLayout);
//
//				_currentShader->_pushConstants.push_back(info);
//
//				addPushConstantRangeForConstantBuffer(plSignature, constantBufferTypeLayout);
//			} break;
//		}
//	}
//
//	void ShaderTransformer::addDescriptorSetForParameterBlock(PipelineLayoutSignature& plSignature,
//															  slang::TypeLayoutReflection* paramBlockTypeLayout) {
//		printf("Adding Set for ParameterBlock\n");
//		DescriptorSetSignature newDSSignature;
//		startBuildingDescriptorSetLayout(plSignature, newDSSignature);
//		addRangesForParameterBlockElement(plSignature, newDSSignature, paramBlockTypeLayout->getElementTypeLayout());
//		finishBuildingDescriptorSetLayout(plSignature, newDSSignature);
//	}
//
//	void ShaderTransformer::addPushConstantRangeForConstantBuffer(PipelineLayoutSignature& plSignature,
//																  slang::TypeLayoutReflection* pushConstantBufferTypeLayout) {
//		auto elementTypeLayout = pushConstantBufferTypeLayout->getElementTypeLayout();
//		const uint32_t elementSize = elementTypeLayout->getSize();
//
//		if (elementSize <= 0) return;
//
//		VkPushConstantRange range = {
//				.stageFlags = _currentStageFlags,
//				.offset = 0,
//				.size = elementSize
//		};
//		plSignature.pushes.push_back(range);
//	}
}