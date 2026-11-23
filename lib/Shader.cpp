//
// Created by Hayden Rivas on 11/1/25.
//

#include "ShaderReflection.h"
#include "HelperMacros.h"
#include "Logger.h"
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_set>

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>
#include <spirv_reflect.h>

#include "mythril/Objects.h"

namespace mythril {
	// SPIR-V names are optional, a stripped or unnamed member reflects as null
	static const char* SpvName(const char* name) { return name ? name : ""; }

	using namespace detail;

	const char* FieldKindToString(FieldKind fieldKind) {
		switch (fieldKind) {
			case FieldKind::OpaqueHandle:
				return "OpaqueHandle";
			case FieldKind::Scalar:
				return "Scalar";
			case FieldKind::Vector:
				return "Vector";
			case FieldKind::Matrix:
				return "Matrix";
			case FieldKind::Array:
				return "Array";
			case FieldKind::Struct:
				return "Struct";
			case FieldKind::Pointer:
				return "Pointer";
			case FieldKind::Unknown:
				return "Unknown";
		}
	}
	const char* ScalarKindToString(ScalarKind scalarKind) {
		switch (scalarKind) {
			case ScalarKind::None:
				return "None";
			case ScalarKind::Int:
				return "Int";
			case ScalarKind::UInt:
				return "UInt";
			case ScalarKind::Float:
				return "Float";
			case ScalarKind::Double:
				return "Double";
			case ScalarKind::Bool:
				return "Bool";
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
				return MatrixDims{rows, cols};
			}
		}
		return std::nullopt;
	}

	static bool DetectMatrixType(const SpvReflectTypeDescription& typeDesc) {
		if (typeDesc.struct_type_description) {
			const std::string s = SpvName(typeDesc.type_name);
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
			case SpvOpTypeBool:
				return ScalarKind::Bool;
			default:
				break;
		}
		UNREACHABLE("Something went horribly wrong, this should never happen!");
	}

	static void CollectFieldsRecursively(std::vector<FieldInfo>& fields, const SpvReflectBlockVariable& blockVar) {
		for (unsigned int i = 0; i < blockVar.member_count; i++) {
			// aliases
			const SpvReflectBlockVariable& memberVar = blockVar.members[i];
			const SpvReflectTypeDescription& memberType = *memberVar.type_description;

			FieldInfo field;
			field.size = memberVar.size;
			field.offset = memberVar.offset;
			field.varName = SpvName(memberVar.name);
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
					field.typeName = SpvName(memberType.type_name);
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
					std::optional<MatrixDims> extracted_dims = ExtractMatrixDims(SpvName(memberType.type_name));
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


	static AllocatedShader::DescriptorBindingInfo GatherDescriptorBindingInformation(const SpvReflectDescriptorBinding& descriptorBind) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *descriptorBind.type_description;
		AllocatedShader::DescriptorBindingInfo info = {};
		// descriptor info
		info.descriptorType = static_cast<VkDescriptorType>(descriptorBind.descriptor_type);
		info.descriptorCount = descriptorBind.count;
		info.setIndex = descriptorBind.set;
		info.bindingIndex = descriptorBind.binding;
		// top level info
		info.typeName = SpvName(blockType.type_name);
		info.varName = SpvName(descriptorBind.name);

		CollectFieldsRecursively(info.fields, descriptorBind.block);
		return info;
	}


	static AllocatedShader::PushConstantInfo GatherPushConstantInformation(const SpvReflectBlockVariable& blockVar) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *blockVar.type_description;
		AllocatedShader::PushConstantInfo info = {};
		// top level info
		info.size = blockVar.size;
		info.offset = blockVar.offset;
		info.varName = SpvName(blockVar.name);
		info.typeName = SpvName(blockType.type_name);

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

	void PatchSpecConstants(uint32_t*& code, size_t& size) {
		SpvReflectShaderModule spirv_module = {};
		SpvReflectResult spv_result = spvReflectCreateShaderModule(size, code, &spirv_module);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "spvReflectCreateShaderModule failed for patching!");

		// we want to get the first descriptor set and ensure that its set index is 0
		// if not then we shift all descriptor sets over by its set index until its at 0
		// we might want to check just the first SpvReflectDescriptorSet, however idk if they are ordered
		uint32_t lowest_set_index = std::numeric_limits<uint32_t>::max();
		for (int i = 0; i < spirv_module.descriptor_set_count; i++) {
			const SpvReflectDescriptorSet& spvSet = spirv_module.descriptor_sets[i];
			if (lowest_set_index > spvSet.set)
				lowest_set_index = spvSet.set;
		}
		if (spirv_module.descriptor_set_count > 0 && lowest_set_index != 0) {
			// now we know we have to apply the patch
			// we loop through every descriptor set and shift their set index down by lowest set
			for (int i = 0; i < spirv_module.descriptor_set_count; i++) {
				SpvReflectDescriptorSet& spvSet = spirv_module.descriptor_sets[i];
				spvReflectChangeDescriptorSetNumber(&spirv_module, &spvSet, spvSet.set - lowest_set_index);
			}
			size = spvReflectGetCodeSize(&spirv_module);
			memcpy(code, spvReflectGetCode(&spirv_module), size);
		}
		spvReflectDestroyShaderModule(&spirv_module);
	}

	ReflectionResult ReflectSPIRV(const uint32_t* code, size_t size) {
		SpvReflectShaderModule spvReflectModule = {};
		SpvReflectResult spvResult = spvReflectCreateShaderModule(size, code, &spvReflectModule);
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Initial reflection of the SpvReflectShaderModule failed!");

		// define return value
		ReflectionResult result = {};

#ifdef DEBUG
		std::unordered_map<VkShaderStageFlags, int> numOfStageEntryPoints;
#endif
		VkShaderStageFlags vkShaderStageFlags = 0;
		const uint32_t entry_point_count = spvReflectModule.entry_point_count;
		for (int i_entryPoint = 0; i_entryPoint < entry_point_count; i_entryPoint++) {
			SpvReflectEntryPoint spvReflectedEntryPoint = spvReflectModule.entry_points[i_entryPoint];
			auto vk_shader_stage_flag = static_cast<VkShaderStageFlagBits>(spvReflectedEntryPoint.shader_stage);
#ifdef DEBUG
			// a raytracing pipeline picks its shaders by entry point, so several per stage is expected
			constexpr VkShaderStageFlags kRayTracingStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
			                                                 VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
			if (!(vk_shader_stage_flag & kRayTracingStages)) {
				numOfStageEntryPoints[vk_shader_stage_flag]++;
				ASSERT_MSG(
				        numOfStageEntryPoints[vk_shader_stage_flag] < 2,
				        "You cannot have more than 1 entrypoint for the same shader stage! '{}' is the second entrypoint that targets '{}'",
				        spvReflectedEntryPoint.name,
				        string_VkShaderStageFlagBits(vk_shader_stage_flag)
				);
			}
#endif
			vkShaderStageFlags |= vk_shader_stage_flag;
		}

		// collect specialization constants
		uint32_t sc_count = 0;
		spvResult = spvReflectEnumerateSpecializationConstants(&spvReflectModule, &sc_count, nullptr);
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Specialization Constants for count.");

		std::vector<SpvReflectSpecializationConstant*> constants(sc_count);
		spvResult = spvReflectEnumerateSpecializationConstants(&spvReflectModule, &sc_count, constants.data());
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Specialization Constants for data.");

		for (const SpvReflectSpecializationConstant* reflected_constant: constants) {
			// safely derefrence
			const SpvReflectTypeDescription& spvConstantTyoe = *reflected_constant->type_description;
			SpecializationConstant info = {};

			info.varName = SpvName(reflected_constant->name);
			info.typeName = "Scalar";
			info.scalarKind = ResolveScalarKindFromType(spvConstantTyoe);
			info.id = reflected_constant->constant_id;

			result.specializationInfo.specializationConstants.emplace_back(info);
			result.specializationInfo.nameToID.insert({info.varName, info.id});
		}

		// collect descriptor sets info
		uint32_t ds_count = 0;
		spvResult = spvReflectEnumerateDescriptorSets(&spvReflectModule, &ds_count, nullptr);
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for count.");

		std::vector<SpvReflectDescriptorSet*> sets(ds_count);
		spvResult = spvReflectEnumerateDescriptorSets(&spvReflectModule, &ds_count, sets.data());
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for data.");

		result.retrievedDescriptorSets.reserve(sets.size());

		for (int set_index = 0; set_index < sets.size(); set_index++) {
			const SpvReflectDescriptorSet* spvReflectedSet = sets[set_index];

			AllocatedShader::DescriptorSetInfo set_info = {};
			set_info.setIndex = spvReflectedSet->set;

			bool isBindless = false;
			for (int binding_index = 0; binding_index < spvReflectedSet->binding_count; binding_index++) {
				const SpvReflectDescriptorBinding* spvReflectedBinding = spvReflectedSet->bindings[binding_index];

				if (strcmp(SpvName(spvReflectedBinding->name), "__slang_resource_heap") == 0) {
					isBindless = true;
					continue;
				}

				ASSERT_MSG(
				        false,
				        "Shader declares non-bindless descriptor binding '{}' at set={}, binding={}. "
				        "Mythril is fully bindless: use Ptr<T> in push constants or DescriptorHandle<T>.",
				        SpvName(spvReflectedBinding->name),
				        spvReflectedSet->set,
				        spvReflectedBinding->binding
				);
			}

			ASSERT_MSG(
			        isBindless,
			        "Shader declares descriptor set={} with no bindless heap. "
			        "Mythril is fully bindless: only Slang's __slang_resource_heap is permitted.",
			        spvReflectedSet->set
			);

			result.pipelineLayoutSignature.bindlessSetIndex = spvReflectedSet->set;
			if (!set_info.bindingInfos.empty())
				result.retrievedDescriptorSets.push_back(set_info);
		}

		// collect push constant info
		uint32_t pc_count = 0;
		spvResult = spvReflectEnumeratePushConstantBlocks(&spvReflectModule, &pc_count, nullptr);
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate PushConstant Blocks for count.");

		std::vector<SpvReflectBlockVariable*> spvBlocks(pc_count);
		spvResult = spvReflectEnumeratePushConstantBlocks(&spvReflectModule, &pc_count, spvBlocks.data());
		ASSERT_MSG(spvResult == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate PushConstant Blocks for count.");

		std::vector<VkPushConstantRange> pc_ranges(spvBlocks.size());
		result.retrivedPushConstants.reserve(spvBlocks.size());
		for (int i_block = 0; i_block < spvBlocks.size(); i_block++) {
			const SpvReflectBlockVariable* spvBlock = spvBlocks[i_block];

			VkPushConstantRange pc_range = {};
			pc_range.size = spvBlock->size;
			pc_range.offset = spvBlock->offset;
			pc_range.stageFlags = vkShaderStageFlags;
			pc_ranges[i_block] = pc_range;

			result.retrivedPushConstants.emplace_back(GatherPushConstantInformation(*spvBlock));
		}
		result.pipelineLayoutSignature.pushes = std::move(pc_ranges);

		spvReflectDestroyShaderModule(&spvReflectModule);

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
#ifdef DEBUG
	namespace {
		constexpr uint8_t kReadBit = 1;
		constexpr uint8_t kWriteBit = 2;
		constexpr uint8_t kUnknownBit = 4;
		constexpr uint32_t kNoClass = UINT32_MAX;

		// byte ranges of the push block a value comes from
		struct ByteRange {
			uint32_t lo;
			uint32_t hi;
			bool operator==(const ByteRange&) const = default;
		};
		using Tag = std::vector<ByteRange>;

		// forward walk from the push block to the image, pointer and atomic ops that use it
		class PushAccessWalk {
		public:
			PushAccessWalk(const std::span<const uint32_t> words, std::vector<PushResourceField>& fields) : _fields(fields), _bits(fields.size(), 0) {
				_bound = words[3];
				_typeOp.assign(_bound, SpvOpNop);
				_typeArgs.assign(_bound, {0, 0});
				_resultType.assign(_bound, 0);
				_variableClass.assign(_bound, kNoClass);
				_root.assign(_bound, 0);
				_tags.assign(_bound, {});
				_contents.assign(_bound, {});
				if (!parse(words)) {
					std::ranges::fill(_bits, kUnknownBit);
					return;
				}
				for (uint32_t pass = 0; pass < 64; ++pass) {
					_changed = false;
					for (const std::span<const uint32_t> inst: _body)
						transfer(inst);
					if (!_changed)
						return;
				}
				std::ranges::fill(_bits, kUnknownBit);
			}
			void write() const {
				for (size_t i = 0; i < _fields.size(); ++i)
					_fields[i].access = _bits[i] & kUnknownBit ? PushResourceField::Access::Unknown : static_cast<PushResourceField::Access>(_bits[i]);
			}

		private:
			bool parse(const std::span<const uint32_t> words) {
				bool inFunction = false;
				for (size_t i = 5; i < words.size();) {
					const uint32_t count = words[i] >> 16;
					if (count == 0 || i + count > words.size())
						return false;
					const std::span<const uint32_t> w = words.subspan(i, count);
					i += count;
					const auto op = static_cast<SpvOp>(w[0] & 0xffff);
					const auto id = [&](const size_t k) { return k < w.size() && w[k] < _bound ? w[k] : 0u; };
					switch (op) {
						case SpvOpExtInstImport:
							if (std::string_view(reinterpret_cast<const char*>(&w[2])).starts_with("NonSemantic."))
								_nonSemanticSets.insert(w[1]);
							break;
						case SpvOpDecorate:
							if (count >= 4 && w[2] == SpvDecorationArrayStride)
								_arrayStrides[w[1]] = w[3];
							break;
						case SpvOpMemberDecorate:
							if (count >= 5 && w[3] == SpvDecorationOffset)
								_memberOffsets[(uint64_t{w[1]} << 32) | w[2]] = w[4];
							break;
						case SpvOpTypeInt:
						case SpvOpTypeFloat:
						case SpvOpTypeVector:
						case SpvOpTypeMatrix:
						case SpvOpTypeArray:
						case SpvOpTypeRuntimeArray:
						case SpvOpTypePointer:
							if (id(1)) {
								_typeOp[w[1]] = op;
								_typeArgs[w[1]] = {count > 2 ? w[2] : 0, count > 3 ? w[3] : 0};
							}
							break;
						case SpvOpTypeStruct:
							if (id(1)) {
								_typeOp[w[1]] = op;
								_structMembers[w[1]].assign(w.begin() + 2, w.end());
							}
							break;
						case SpvOpConstant:
							if (count >= 4)
								_constants[w[2]] = w[3];
							break;
						case SpvOpVariable:
							if (!id(2))
								return false;
							_resultType[w[2]] = w[1];
							_variableClass[w[2]] = w[3];
							if (w[3] == SpvStorageClassPushConstant)
								_tags[w[2]] = {{0, sizeOf(pointee(w[1]))}};
							if (w[3] == SpvStorageClassFunction || w[3] == SpvStorageClassPrivate)
								_root[w[2]] = w[2];
							break;
						case SpvOpFunction:
							inFunction = true;
							break;
						case SpvOpFunctionEnd:
							inFunction = false;
							break;
						default:
							break;
					}
					if (inFunction)
						_body.push_back(w);
				}
				return true;
			}

			uint32_t pointee(const uint32_t pointerType) const { return _typeOp[pointerType] == SpvOpTypePointer ? _typeArgs[pointerType][1] : 0; }
			uint32_t storageClass(const uint32_t value) const {
				const uint32_t type = _resultType[value];
				return _typeOp[type] == SpvOpTypePointer ? _typeArgs[type][0] : kNoClass;
			}
			std::optional<uint32_t> constant(const uint32_t value) const {
				const auto it = _constants.find(value);
				return it == _constants.end() ? std::nullopt : std::optional(it->second);
			}
			std::optional<uint32_t> memberOffset(const uint32_t type, const uint32_t member) const {
				const auto it = _memberOffsets.find((uint64_t{type} << 32) | member);
				return it == _memberOffsets.end() ? std::nullopt : std::optional(it->second);
			}
			std::optional<uint32_t> arrayStride(const uint32_t type) const {
				const auto it = _arrayStrides.find(type);
				return it == _arrayStrides.end() ? std::nullopt : std::optional(it->second);
			}
			uint32_t sizeOf(const uint32_t type) const {
				if (type >= _bound)
					return 0;
				const auto [a, b] = _typeArgs[type];
				switch (_typeOp[type]) {
					case SpvOpTypeInt:
					case SpvOpTypeFloat:
						return a / 8;
					case SpvOpTypeVector:
					case SpvOpTypeMatrix:
						return b * sizeOf(a);
					case SpvOpTypeArray:
						return constant(b).value_or(0) * arrayStride(type).value_or(sizeOf(a));
					case SpvOpTypeRuntimeArray:
						return UINT32_MAX / 2;
					case SpvOpTypePointer:
						return 8;
					case SpvOpTypeStruct: {
						uint32_t end = 0;
						const std::vector<uint32_t>& members = _structMembers.at(type);
						for (uint32_t m = 0; m < members.size(); ++m)
							end = std::max(end, memberOffset(type, m).value_or(end) + sizeOf(members[m]));
						return end;
					}
					default:
						return 0;
				}
			}
			// narrows a range of a value of 'type' to its element 'index', a dynamic index gives every element
			bool step(const uint32_t type, const ByteRange range, const std::optional<uint32_t> index, std::vector<std::pair<uint32_t, ByteRange>>& out) const {
				const auto [a, b] = _typeArgs[type];
				switch (_typeOp[type]) {
					case SpvOpTypeStruct: {
						const std::vector<uint32_t>& members = _structMembers.at(type);
						const std::optional<uint32_t> offset = index && *index < members.size() ? memberOffset(type, *index) : std::nullopt;
						if (!offset)
							return false;
						out.emplace_back(members[*index], ByteRange{range.lo + *offset, range.lo + *offset + sizeOf(members[*index])});
						return true;
					}
					case SpvOpTypeArray:
					case SpvOpTypeVector: {
						// scalar layout packs elements, so a missing ArrayStride means the element size
						const uint32_t stride = _typeOp[type] == SpvOpTypeVector ? sizeOf(a) : arrayStride(type).value_or(sizeOf(a));
						const uint32_t length = _typeOp[type] == SpvOpTypeVector ? b : constant(b).value_or(0);
						const uint32_t first = index.value_or(0);
						const uint32_t last = index ? *index + 1 : length;
						if (stride == 0 || last > length)
							return false;
						for (uint32_t element = first; element < last; ++element)
							out.emplace_back(a, ByteRange{range.lo + element * stride, range.lo + (element + 1) * stride});
						return true;
					}
					case SpvOpTypeMatrix:
						out.emplace_back(a, range);
						return true;
					default:
						return false;
				}
			}
			Tag narrow(const uint32_t type, const Tag& tag, const std::span<const uint32_t> indices, const bool literal) {
				std::vector<std::pair<uint32_t, ByteRange>> current;
				std::vector<std::pair<uint32_t, ByteRange>> next;
				for (const ByteRange& range: tag)
					current.emplace_back(type, range);
				for (const uint32_t index: indices) {
					next.clear();
					for (const auto& [elementType, range]: current) {
						if (!step(elementType, range, literal ? std::optional(index) : constant(index), next)) {
							effect(tag, kUnknownBit);
							return {};
						}
					}
					std::swap(current, next);
				}
				Tag out;
				for (const ByteRange& range: current | std::views::values)
					out.push_back(range);
				return out;
			}
			void merge(Tag& into, const Tag& tag) {
				for (const ByteRange& range: tag) {
					if (std::ranges::find(into, range) == into.end()) {
						into.push_back(range);
						_changed = true;
					}
				}
			}
			void add(const uint32_t value, const Tag& tag) { merge(_tags[value], tag); }
			void effect(const Tag& tag, const uint8_t bit) {
				for (const ByteRange& range: tag)
					for (size_t i = 0; i < _fields.size(); ++i)
						if (_fields[i].offset >= range.lo && _fields[i].offset < range.hi)
							_bits[i] |= bit;
			}
			const Tag& tagOf(const uint32_t value) const {
				static const Tag kEmpty;
				return value < _bound ? _tags[value] : kEmpty;
			}
			// a local holding more than one field would blur them on load, so only single fields are followed
			bool holdsOneField(const Tag& tag) const {
				size_t hits = 0;
				for (const ByteRange& range: tag)
					for (const PushResourceField& field: _fields)
						hits += field.offset >= range.lo && field.offset < range.hi;
				return hits <= 1;
			}

			void transfer(const std::span<const uint32_t> w) {
				const auto op = static_cast<SpvOp>(w[0] & 0xffff);
				const auto count = static_cast<uint32_t>(w.size());
				switch (op) {
					case SpvOpAccessChain:
					case SpvOpInBoundsAccessChain: {
						_resultType[w[2]] = w[1];
						const uint32_t base = w[3];
						if (_root[base])
							_root[w[2]] = _root[base];
						if (!tagOf(base).empty())
							add(w[2], storageClass(base) == SpvStorageClassPushConstant ? narrow(pointee(_resultType[base]), tagOf(base), w.subspan(4), false) : tagOf(base));
						for (uint32_t k = 4; k < count; ++k) {
							if (tagOf(w[k]).empty())
								continue;
							if (_variableClass[base] == SpvStorageClassUniformConstant)
								add(w[2], tagOf(w[k]));
							else
								effect(tagOf(w[k]), kUnknownBit);
						}
						return;
					}
					case SpvOpPtrAccessChain:
					case SpvOpInBoundsPtrAccessChain:
						_resultType[w[2]] = w[1];
						if (_root[w[3]])
							_root[w[2]] = _root[w[3]];
						add(w[2], tagOf(w[3]));
						for (uint32_t k = 4; k < count; ++k)
							effect(tagOf(w[k]), kUnknownBit);
						return;
					case SpvOpLoad: {
						_resultType[w[2]] = w[1];
						const uint32_t pointer = w[3];
						if (_root[pointer])
							add(w[2], _contents[_root[pointer]]);
						if (tagOf(pointer).empty())
							return;
						switch (storageClass(pointer)) {
							case SpvStorageClassPhysicalStorageBuffer:
								effect(tagOf(pointer), kReadBit);
								return;
							case SpvStorageClassPushConstant:
							case SpvStorageClassUniformConstant:
								add(w[2], tagOf(pointer));
								return;
							default:
								effect(tagOf(pointer), kUnknownBit);
								return;
						}
					}
					case SpvOpStore: {
						const uint32_t pointer = w[1];
						const Tag& object = tagOf(w[2]);
						if (!tagOf(pointer).empty())
							effect(tagOf(pointer), storageClass(pointer) == SpvStorageClassPhysicalStorageBuffer ? kWriteBit : kUnknownBit);
						if (object.empty())
							return;
						if (_root[pointer] && holdsOneField(object))
							merge(_contents[_root[pointer]], object);
						else
							effect(object, kUnknownBit);
						return;
					}
					case SpvOpCompositeExtract:
						_resultType[w[2]] = w[1];
						if (!tagOf(w[3]).empty())
							add(w[2], narrow(_resultType[w[3]], tagOf(w[3]), w.subspan(4), true));
						return;
					case SpvOpCopyObject:
					case SpvOpCopyLogical:
					case SpvOpBitcast:
					case SpvOpConvertUToPtr:
					case SpvOpConvertPtrToU:
					case SpvOpImage:
						_resultType[w[2]] = w[1];
						if (_root[w[3]])
							_root[w[2]] = _root[w[3]];
						add(w[2], tagOf(w[3]));
						return;
					case SpvOpPhi:
						_resultType[w[2]] = w[1];
						for (uint32_t k = 3; k < count; k += 2)
							add(w[2], tagOf(w[k]));
						return;
					case SpvOpSelect:
						_resultType[w[2]] = w[1];
						effect(tagOf(w[3]), kUnknownBit);
						add(w[2], tagOf(w[4]));
						add(w[2], tagOf(w[5]));
						return;
					case SpvOpSampledImage:
						_resultType[w[2]] = w[1];
						add(w[2], tagOf(w[3]));
						add(w[2], tagOf(w[4]));
						return;
					case SpvOpImageTexelPointer:
						_resultType[w[2]] = w[1];
						add(w[2], tagOf(w[3]));
						return;
					case SpvOpImageSampleImplicitLod:
					case SpvOpImageSampleExplicitLod:
					case SpvOpImageSampleDrefImplicitLod:
					case SpvOpImageSampleDrefExplicitLod:
					case SpvOpImageSampleProjImplicitLod:
					case SpvOpImageSampleProjExplicitLod:
					case SpvOpImageSampleProjDrefImplicitLod:
					case SpvOpImageSampleProjDrefExplicitLod:
					case SpvOpImageFetch:
					case SpvOpImageGather:
					case SpvOpImageDrefGather:
					case SpvOpImageRead:
					case SpvOpImageSparseSampleImplicitLod:
					case SpvOpImageSparseSampleExplicitLod:
					case SpvOpImageSparseSampleDrefImplicitLod:
					case SpvOpImageSparseSampleDrefExplicitLod:
					case SpvOpImageSparseFetch:
					case SpvOpImageSparseGather:
					case SpvOpImageSparseDrefGather:
					case SpvOpImageSparseRead:
						effect(tagOf(w[3]), kReadBit);
						return;
					case SpvOpImageWrite:
						effect(tagOf(w[1]), kWriteBit);
						return;
					case SpvOpImageQueryFormat:
					case SpvOpImageQueryOrder:
					case SpvOpImageQuerySizeLod:
					case SpvOpImageQuerySize:
					case SpvOpImageQueryLod:
					case SpvOpImageQueryLevels:
					case SpvOpImageQuerySamples:
						return;
					case SpvOpAtomicLoad:
						effect(tagOf(w[3]), kReadBit);
						return;
					case SpvOpAtomicStore:
						effect(tagOf(w[1]), kWriteBit);
						effect(tagOf(w[4]), kUnknownBit);
						return;
					case SpvOpAtomicExchange:
					case SpvOpAtomicCompareExchange:
					case SpvOpAtomicCompareExchangeWeak:
					case SpvOpAtomicIIncrement:
					case SpvOpAtomicIDecrement:
					case SpvOpAtomicIAdd:
					case SpvOpAtomicISub:
					case SpvOpAtomicSMin:
					case SpvOpAtomicUMin:
					case SpvOpAtomicSMax:
					case SpvOpAtomicUMax:
					case SpvOpAtomicAnd:
					case SpvOpAtomicOr:
					case SpvOpAtomicXor:
					case SpvOpAtomicFlagTestAndSet:
					case SpvOpAtomicFMinEXT:
					case SpvOpAtomicFMaxEXT:
					case SpvOpAtomicFAddEXT:
						effect(tagOf(w[3]), kReadBit | kWriteBit);
						for (uint32_t k = 4; k < count; ++k)
							effect(tagOf(w[k]), kUnknownBit);
						return;
					case SpvOpAtomicFlagClear:
						effect(tagOf(w[1]), kWriteBit);
						return;
					case SpvOpExtInst:
						if (_nonSemanticSets.contains(w[3]))
							return;
						break;
					default:
						break;
				}
				// an op the walk does not model loses the trace of every field it touches
				for (uint32_t k = 1; k < count; ++k)
					effect(tagOf(w[k]), kUnknownBit);
			}

			std::vector<PushResourceField>& _fields;
			std::vector<uint8_t> _bits;
			uint32_t _bound = 0;
			std::vector<SpvOp> _typeOp;
			std::vector<std::array<uint32_t, 2>> _typeArgs;
			std::unordered_map<uint32_t, std::vector<uint32_t>> _structMembers;
			std::unordered_map<uint64_t, uint32_t> _memberOffsets;
			std::unordered_map<uint32_t, uint32_t> _arrayStrides;
			std::unordered_map<uint32_t, uint32_t> _constants;
			std::unordered_set<uint32_t> _nonSemanticSets;
			std::vector<uint32_t> _resultType;
			std::vector<uint32_t> _variableClass;
			// the Function or Private variable a pointer points into
			std::vector<uint32_t> _root;
			std::vector<Tag> _tags;
			// what was stored into each Function or Private variable
			std::vector<Tag> _contents;
			std::vector<std::span<const uint32_t>> _body;
			bool _changed = false;
		};
	} // namespace

	void AnalyzePushAccess(const uint32_t* code, const size_t size, std::vector<PushResourceField>& fields) {
		const std::span<const uint32_t> words(code, size / sizeof(uint32_t));
		if (fields.empty() || words.size() < 5 || words[0] != SpvMagicNumber)
			return;
		PushAccessWalk(words, fields).write();
	}
#endif
} // namespace mythril
