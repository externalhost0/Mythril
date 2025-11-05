//
// Created by Hayden Rivas on 11/1/25.
//

#include "Shader.h"
#include "HelperMacros.h"
#include "Logger.h"

#include <iostream>
#include <slang/slang-com-ptr.h>

namespace mythril {

	void PipelineLayoutBuilder::clear() {
		_dslBuilders.clear();
		_vkPCRs.clear();
	}
	PipelineLayoutBuilder& PipelineLayoutBuilder::addDescriptorSetLayoutBuilder(DescriptorSetLayoutBuilder& dsBuilder) {
		_dslBuilders.push_back(std::move(dsBuilder));
		return *this;
	}
	PipelineLayoutBuilder& PipelineLayoutBuilder::addPushConstantRange(VkPushConstantRange pcRange) {
		_vkPCRs.push_back(pcRange);
		return *this;
	}
	VkPipelineLayout PipelineLayoutBuilder::build(VkDevice device) {
		_builtDSLs.reserve(_dslBuilders.size());
		for (auto& builder : _dslBuilders) {
			_builtDSLs.push_back(builder.build(device));
		}
		VkPipelineLayoutCreateInfo pl_ci = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.setLayoutCount = static_cast<uint32_t>(_builtDSLs.size()),
				.pSetLayouts = _builtDSLs.data(),
				.pushConstantRangeCount = static_cast<uint32_t>(_vkPCRs.size()),
				.pPushConstantRanges = _vkPCRs.data(),
		};
		VkPipelineLayout pl = VK_NULL_HANDLE;
		VK_CHECK(vkCreatePipelineLayout(device, &pl_ci, nullptr, &pl));
		// clear the builder so we can reuse it if needed immediately
//		this->clear();
		return pl;
	}

	void DescriptorSetLayoutBuilder::clear() {
		_vkDescriptorRanges.clear();
	}

	DescriptorSetLayoutBuilder& DescriptorSetLayoutBuilder::addBinding(uint32_t bindIndex, VkDescriptorType descriptorType, VkShaderStageFlags stages, uint32_t descriptorCount) {
		VkDescriptorSetLayoutBinding binding = {
				.binding = bindIndex,
				.descriptorType = descriptorType,
				.descriptorCount = descriptorCount,
				.stageFlags = stages,
				.pImmutableSamplers = nullptr,
		};
		_vkDescriptorRanges.push_back(binding);
		return *this;
	}

	uint32_t DescriptorSetLayoutBuilder::getBindingCount() const {
		return this->_vkDescriptorRanges.size();
	}
	VkDescriptorSetLayout DescriptorSetLayoutBuilder::build(VkDevice vkDevice) {
		VkDescriptorSetLayoutCreateInfo dsl_ci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.bindingCount = static_cast<uint32_t>(_vkDescriptorRanges.size()),
				.pBindings = _vkDescriptorRanges.data(),
		};
		VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
		VK_CHECK(vkCreateDescriptorSetLayout(vkDevice, &dsl_ci, nullptr, &dsl));
		this->clear();
		return dsl;
	}


	const char* BindingTypeToString(slang::BindingType bindingRangeType) {
		switch (bindingRangeType) {
			case slang::BindingType::Unknown:                         return "Unknown";
			case slang::BindingType::Sampler:                         return "Sampler";
			case slang::BindingType::Texture:                         return "Texture";
			case slang::BindingType::ConstantBuffer:                  return "ConstantBuffer";
			case slang::BindingType::ParameterBlock:                  return "ParameterBlock";
			case slang::BindingType::TypedBuffer:                     return "TypedBuffer";
			case slang::BindingType::RawBuffer:                       return "RawBuffer";
			case slang::BindingType::CombinedTextureSampler:          return "CombinedTextureSampler";
			case slang::BindingType::InputRenderTarget:               return "InputRenderTarget";
			case slang::BindingType::InlineUniformData:               return "InlineUniformData";
			case slang::BindingType::RayTracingAccelerationStructure: return "RayTracingAccelerationStructure";
			case slang::BindingType::VaryingInput:                    return "VaryingInput";
			case slang::BindingType::VaryingOutput:                   return "VaryingOutput";
			case slang::BindingType::ExistentialValue:                return "ExistentialValue";
			case slang::BindingType::PushConstant:                    return "PushConstant";
			case slang::BindingType::MutableFlag:                     return "MutableFlag";
			case slang::BindingType::MutableTexture:                  return "MutableTexture";
			case slang::BindingType::MutableTypedBuffer:              return "MutableTypedBuffer";
			case slang::BindingType::MutableRawBuffer:                return "MutableRawBuffer";
			case slang::BindingType::BaseMask:                        return "BaseMask";
			case slang::BindingType::ExtMask:                         return "ExtMask";
		}
	}
	VkDescriptorType mapToDescriptorType(slang::BindingType bindingRangeType) {
		switch(bindingRangeType) {
			case slang::BindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
			case slang::BindingType::Texture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			case slang::BindingType::MutableTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case slang::BindingType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

			case slang::BindingType::MutableRawBuffer:
			case slang::BindingType::RawBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

			case slang::BindingType::TypedBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			case slang::BindingType::MutableTypedBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;

			case slang::BindingType::InputRenderTarget: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			case slang::BindingType::CombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case slang::BindingType::RayTracingAccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			default:
				ASSERT_MSG(false, "Unsupported binding range type: {}", BindingTypeToString(bindingRangeType));
		}
	}
	const char* ResourceShapeToString(SlangResourceShape shape) {
		switch (shape) {
			case SLANG_TEXTURE_1D:                   return "TEXTURE_1D";
			case SLANG_TEXTURE_2D:                   return "TEXTURE_2D";
			case SLANG_TEXTURE_3D:                   return "TEXTURE_3D";
			case SLANG_TEXTURE_CUBE:                 return "TEXTURE_CUBE";
			case SLANG_TEXTURE_BUFFER:               return "TEXTURE_BUFFER";
			case SLANG_STRUCTURED_BUFFER:            return "STRUCTURED_BUFFER";
			case SLANG_BYTE_ADDRESS_BUFFER:          return "BYTE_ADDRESS_BUFFER";
			case SLANG_RESOURCE_UNKNOWN:             return "RESOURCE_UNKNOWN";
			case SLANG_ACCELERATION_STRUCTURE:       return "ACCELERATION_STRUCTURE";
			case SLANG_TEXTURE_SUBPASS:              return "TEXTURE_SUBPASS";
			case SLANG_TEXTURE_FEEDBACK_FLAG:        return "TEXTURE_FEEDBACK_FLAG";
			case SLANG_TEXTURE_SHADOW_FLAG:          return "TEXTURE_SHADOW_FLAG";
			case SLANG_TEXTURE_ARRAY_FLAG:           return "TEXTURE_ARRAY_FLAG";
			case SLANG_TEXTURE_MULTISAMPLE_FLAG:     return "TEXTURE_MULTISAMPLE_FLAG";
			case SLANG_TEXTURE_1D_ARRAY:             return "TEXTURE_1D_ARRAY";
			case SLANG_TEXTURE_2D_ARRAY:             return "TEXTURE_2D_ARRAY";
			case SLANG_TEXTURE_CUBE_ARRAY:           return "TEXTURE_CUBE_ARRAY";

			case SLANG_RESOURCE_BASE_SHAPE_MASK:     return "RESOURCE_BASE_SHAPE_MASK";
			case SLANG_RESOURCE_NONE:                return "SLANG_RESOURCE_NONE";
			case SLANG_RESOURCE_EXT_SHAPE_MASK:      return "SLANG_RESOURCE_EXT_SHAPE_MASK";
			case SLANG_TEXTURE_2D_MULTISAMPLE:       return "SLANG_TEXTURE_2D_MULTISAMPLE";
			case SLANG_TEXTURE_2D_MULTISAMPLE_ARRAY: return "SLANG_TEXTURE_2D_MULTISAMPLE_ARRAY";
			case SLANG_TEXTURE_SUBPASS_MULTISAMPLE:  return "SLANG_TEXTURE_SUBPASS_MULTISAMPLE";
		}
	}
	const char* ResourceAccessToString(SlangResourceAccess access) {
		switch (access) {
			case SLANG_RESOURCE_ACCESS_NONE:           return "RESOURCE_ACCESS_NONE";
			case SLANG_RESOURCE_ACCESS_READ:           return "RESOURCE_ACCESS_READ";
			case SLANG_RESOURCE_ACCESS_READ_WRITE:     return "RESOURCE_ACCESS_READ_WRITE";
			case SLANG_RESOURCE_ACCESS_RASTER_ORDERED: return "RESOURCE_ACCESS_RASTER_ORDERED";
			case SLANG_RESOURCE_ACCESS_APPEND:         return "RESOURCE_ACCESS_APPEND";
			case SLANG_RESOURCE_ACCESS_CONSUME:        return "RESOURCE_ACCESS_CONSUME";
			case SLANG_RESOURCE_ACCESS_WRITE:          return "RESOURCE_ACCESS_WRITE";
			case SLANG_RESOURCE_ACCESS_FEEDBACK:       return "RESOURCE_ACCESS_FEEDBACK";
			case SLANG_RESOURCE_ACCESS_UNKNOWN:        return "RESOURCE_ACCESS_UNKNOWN";
			default: return "UNSUPPORTED";
		}
	}
	const char* TypeKindToString(slang::TypeReflection::Kind typeKind) {
		switch(typeKind) {
			case slang::TypeReflection::Kind::Scalar:               return "Scalar";
			case slang::TypeReflection::Kind::Vector:               return "Vector";
			case slang::TypeReflection::Kind::Matrix:               return "Matrix";
			case slang::TypeReflection::Kind::Array:                return "Array";
			case slang::TypeReflection::Kind::Struct:               return "Struct";
			case slang::TypeReflection::Kind::Resource:             return "Resource";
			case slang::TypeReflection::Kind::SamplerState:         return "SamplerState";
			case slang::TypeReflection::Kind::ConstantBuffer:       return "ConstantBuffer";
			case slang::TypeReflection::Kind::ParameterBlock:       return "ParameterBlock";
			case slang::TypeReflection::Kind::TextureBuffer:        return "TextureBuffer";
			case slang::TypeReflection::Kind::ShaderStorageBuffer:  return "ShaderStorageBuffer";
			case slang::TypeReflection::Kind::Pointer:              return "Pointer";
			case slang::TypeReflection::Kind::DynamicResource:      return "DynamicResource";

			case slang::TypeReflection::Kind::GenericTypeParameter: return "GenericTypeParameter";
			case slang::TypeReflection::Kind::Interface:            return "Interface";
			case slang::TypeReflection::Kind::OutputStream:         return "OutputStream";
			case slang::TypeReflection::Kind::Specialized:          return "Specialized";
			case slang::TypeReflection::Kind::Feedback:             return "Feedback";
			case slang::TypeReflection::Kind::None:                 return "None";
		}
	}

	const char* ScalarTypeToString(slang::TypeReflection::ScalarType scalarType) {
		switch (scalarType) {
			case slang::TypeReflection::None:    return "None";
			case slang::TypeReflection::Void:    return "Void";
			case slang::TypeReflection::Bool:    return "Bool";
			case slang::TypeReflection::Int32:   return "Int32";
			case slang::TypeReflection::UInt32:  return "UInt32";
			case slang::TypeReflection::Int64:   return "Int64";
			case slang::TypeReflection::UInt64:  return "UInt64";
			case slang::TypeReflection::Float16: return "Float16";
			case slang::TypeReflection::Float32: return "Float32";
			case slang::TypeReflection::Float64: return "Float64";
			case slang::TypeReflection::Int8:    return "Int8";
			case slang::TypeReflection::UInt8:   return "UInt8";
			case slang::TypeReflection::Int16:   return "Int16";
			case slang::TypeReflection::UInt16:  return "UInt16";
		}
	}

	const char* MatrixModeToString(SlangMatrixLayoutMode matrixLayoutMode) {
		switch (matrixLayoutMode) {
			case SLANG_MATRIX_LAYOUT_MODE_UNKNOWN: return "MATRIX_LAYOUT_MODE_UNKNOWN";
			case SLANG_MATRIX_LAYOUT_ROW_MAJOR:    return "MATRIX_LAYOUT_MODE_ROW_MAJOR";
			case SLANG_MATRIX_LAYOUT_COLUMN_MAJOR: return "MATRIX_LAYOUT_MODE_COLUMN_MAJOR";
		}
	}

	// Helper to convert variable category to string
	//
	const char* LayoutUnitToString(slang::ParameterCategory category) {
		switch(category) {
			// these first 7 are the ONLY ones we care about using Vulkan
			case slang::ParameterCategory::Uniform:                    return "Uniform";
			case slang::ParameterCategory::DescriptorTableSlot:        return "DescriptorTableSlot";
			case slang::ParameterCategory::SubElementRegisterSpace:    return "SubElementRegisterSpace";
			case slang::ParameterCategory::PushConstantBuffer:         return "PushConstantBuffer";
			case slang::ParameterCategory::SpecializationConstant:     return "SpecializationConstant";
			case slang::ParameterCategory::VaryingInput:               return "VaryingInput";
			case slang::ParameterCategory::VaryingOutput:              return "VaryingOutput";
				// things our application should never run into, as vulkan never uses any of these layout units
			case slang::ParameterCategory::None:                       return "None";
			case slang::ParameterCategory::Mixed:                      return "Mixed";
			case slang::ParameterCategory::ConstantBuffer:             return "ConstantBuffer";
			case slang::ParameterCategory::ShaderResource:             return "ShaderResource";
			case slang::ParameterCategory::UnorderedAccess:            return "UnorderedAccess";
			case slang::ParameterCategory::SamplerState:               return "SamplerState";
			case slang::ParameterCategory::RegisterSpace:              return "RegisterSpace";
			case slang::ParameterCategory::GenericResource:            return "GenericResource";
			case slang::ParameterCategory::RayPayload:                 return "RayPayload";
			case slang::ParameterCategory::HitAttributes:              return "HitAttributes";
			case slang::ParameterCategory::CallablePayload:            return "CallablePayload";
			case slang::ParameterCategory::ShaderRecord:               return "ShaderRecord";
			case slang::ParameterCategory::ExistentialTypeParam:       return "ExistentialTypeParam";
			case slang::ParameterCategory::ExistentialObjectParam:     return "ExistentialObjectParam";
			case slang::ParameterCategory::InputAttachmentIndex:       return "InputAttachmentIndex";
			case slang::ParameterCategory::MetalArgumentBufferElement: return "MetalArgumentBufferElement";
			case slang::ParameterCategory::MetalAttribute:             return "MetalAttribute";
			case slang::ParameterCategory::MetalPayload:               return "MetalPayload";
		}
	}
	const char* SlangStageToString(SlangStage stage) {
		switch (stage) {
			case SLANG_STAGE_NONE:           return "None";
			case SLANG_STAGE_VERTEX:         return "Vertex";
			case SLANG_STAGE_HULL:           return "Hull";
			case SLANG_STAGE_DOMAIN:         return "Domain";
			case SLANG_STAGE_GEOMETRY:       return "Geometry";
			case SLANG_STAGE_FRAGMENT:       return "Fragment";
			case SLANG_STAGE_COMPUTE:        return "Compute";
			case SLANG_STAGE_RAY_GENERATION: return "RayGeneration";
			case SLANG_STAGE_INTERSECTION:   return "Intersection";
			case SLANG_STAGE_ANY_HIT:        return "AnyHit";
			case SLANG_STAGE_CLOSEST_HIT:    return "ClosestHit";
			case SLANG_STAGE_MISS:           return "Miss";
			case SLANG_STAGE_CALLABLE:       return "Callable";
			case SLANG_STAGE_MESH:           return "Mesh";
			case SLANG_STAGE_AMPLIFICATION:  return "Amplification";
			case SLANG_STAGE_COUNT:          return "Count";
		}
	}

	std::string resolveArrayName(slang::TypeReflection* type) {
		if (type->isArray()) {
			return "Array of " + resolveArrayName(type->getElementType());
		}
		return type->getName();
	}
	std::string resolveFullTypeName(slang::TypeLayoutReflection* typeLayout) {
		std::string fullName = resolveArrayName(typeLayout->getType());
		return fullName;
	}

//	void addRangesForParameterBlockElement(PipelineLayoutBuilder& plBuilder, DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* elementTypeLayout);
//	void addAutomaticallyIntroducedUniformBuffer(DescriptorSetLayoutBuilder& dslBuilder);
//
//	void addDescriptorSetForParameterBlock(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* paramBlockTypeLayout);
//	void addPushConstantRangeForConstantBuffer(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* pushConstantBufferTypeLayout);
//
//
//	void addRanges(PipelineLayoutBuilder& plBuilder, DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout);
//
//	void addDescriptorRanges(DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout);
//	void addDescriptorRange(DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout, int relativeSetIndex, int rangeIndex);
//
//	void addSubRanges(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* typeLayout);
//	void addSubRange(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* typeLayout, int subRangeIndex);

	OpaqueKind getOpaqueKind(slang::TypeLayoutReflection* typeLayout) {
		auto type = typeLayout->getType();
		const char* typeName = typeLayout->getName();

		auto kind = type->getKind();
		if (kind == slang::TypeReflection::Kind::Resource) {
			auto shape = type->getResourceShape();
			auto access = type->getResourceAccess();
			bool isReadWrite = (access == SLANG_RESOURCE_ACCESS_READ_WRITE);

			switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
				case SLANG_TEXTURE_1D:
					return isReadWrite ? OpaqueKind::RWTexture1D
									   : OpaqueKind::Texture1D;
				case SLANG_TEXTURE_2D:
					return isReadWrite ? OpaqueKind::RWTexture2D
									   : OpaqueKind::Texture2D;
				case SLANG_TEXTURE_3D:
					return isReadWrite ? OpaqueKind::RWTexture3D
									   : OpaqueKind::Texture3D;
				case SLANG_TEXTURE_CUBE:
					return OpaqueKind::TextureCube;
				default:
					return OpaqueKind::Unknown;
			}
		} else if (kind == slang::TypeReflection::Kind::SamplerState) {
			return OpaqueKind::Sampler;
		}
		return OpaqueKind::Unknown;
	}

	ScalarKind getScalarType(slang::TypeLayoutReflection* typeLayout) {
		auto type = typeLayout->getType();
		auto scalarType = type->getScalarType();

		switch (scalarType) {
			case slang::TypeReflection::ScalarType::Float32:
			case slang::TypeReflection::ScalarType::Float64:
				return ScalarKind::Float;
			case slang::TypeReflection::ScalarType::Int32:
			case slang::TypeReflection::ScalarType::Int16:
			case slang::TypeReflection::ScalarType::Int64:
			case slang::TypeReflection::ScalarType::Int8:
				return ScalarKind::Int;
			case slang::TypeReflection::ScalarType::UInt32:
			case slang::TypeReflection::ScalarType::UInt16:
			case slang::TypeReflection::ScalarType::UInt64:
			case slang::TypeReflection::ScalarType::UInt8:
				return ScalarKind::UInt;
			case slang::TypeReflection::ScalarType::Bool:
				return ScalarKind::Bool;
			default:
				return ScalarKind::None;
		}
	}

	FieldKind getFieldKind(slang::TypeLayoutReflection* typeLayout) {
		auto type = typeLayout->getType();
		auto kind = type->getKind();
		const char* typeName = typeLayout->getName();

		switch (kind) {
			case slang::TypeReflection::Kind::Struct:
				return FieldKind::Struct;
			case slang::TypeReflection::Kind::Array:
				return FieldKind::Array;
			case slang::TypeReflection::Kind::Vector:
				return FieldKind::Vector;
			case slang::TypeReflection::Kind::Matrix:
				return FieldKind::Matrix;
			case slang::TypeReflection::Kind::Scalar:
			default:
				return FieldKind::Scalar;
		}
	}

	void collectFieldsRecursively(std::vector<FieldInfo>& fields, slang::TypeLayoutReflection* typeLayout) {
		unsigned int fieldCount = typeLayout->getFieldCount();
		for (unsigned int i = 0; i < fieldCount; ++i) {
			auto fieldVariableLayout = typeLayout->getFieldByIndex(i);
			auto fieldTypeLayout = fieldVariableLayout->getTypeLayout();

			FieldInfo field;
			field.varName = fieldVariableLayout->getName();
			field.typeName = fieldTypeLayout->getName();
			field.fieldKind = getFieldKind(fieldTypeLayout);
			field.scalarKind = getScalarType(fieldTypeLayout);
			field.size = fieldTypeLayout->getSize();
			field.offset = fieldVariableLayout->getOffset();

			// must be take from variableReflection
			// TODO: remove me before merge
			if (const char* string = fieldVariableLayout->getVariable()->getUserAttributeByIndex(0)->getName()) {
				int hasMark = strcmp(string, "MarkHandle");
				if (hasMark == 0) {
					int val;
					fieldVariableLayout->getVariable()->getUserAttributeByIndex(0)->getArgumentValueInt(0, &val);
					field.opaqueKind = (OpaqueKind)val;
					field.fieldKind = FieldKind::OpaqueHandle;
					field.scalarKind = ScalarKind::None;
					field.typeName = "OpaqueHandle";
				}
			}

			auto type = fieldTypeLayout->getType();
			switch (field.fieldKind) {
				case FieldKind::Vector: {
					field.componentCount = type->getElementCount();
				} break;
				case FieldKind::Matrix: {
					field.rowCount = type->getRowCount();
					field.columnCount = type->getColumnCount();
				} break;
				case FieldKind::Array: {
					field.componentCount = type->getElementCount();  // Array size
					// Recursively collect the array element type
					auto elementTypeLayout = fieldTypeLayout->getElementTypeLayout();
					if (elementTypeLayout && elementTypeLayout->getFieldCount() > 0) {
						collectFieldsRecursively(field.fields, elementTypeLayout);
					}
				} break;
				case FieldKind::Struct: {
					collectFieldsRecursively(field.fields, fieldTypeLayout);
				} break;
				default:
					break;
			}
			fields.push_back(field);
		}
	}

	void ShaderTransformer::startBuildingDescriptorSetLayout(PipelineLayoutBuilder& plBuilder, DescriptorSetLayoutBuilder& dslBuilder) {
		dslBuilder.setIndex = plBuilder._dslBuilders.size();
	}

	void ShaderTransformer::finishBuildingDescriptorSetLayout(PipelineLayoutBuilder& plBuilder, DescriptorSetLayoutBuilder& dslBuilder) {
		if (dslBuilder._vkDescriptorRanges.empty())
			return;
		ASSERT_MSG(dslBuilder.setIndex == plBuilder._dslBuilders.size(), "Set index doesnt align with dsl vector size!");
		plBuilder._dslBuilders.push_back(std::move(dslBuilder));
	}

	void ShaderTransformer::performReflection(Shader& shader, slang::ShaderReflection* reflectionData) {
		_currentShader = &shader;
		PipelineLayoutBuilder plBuilder;
		DescriptorSetLayoutBuilder dslBuilder;
		startBuildingDescriptorSetLayout(plBuilder, dslBuilder);
		addRangesForParameterBlockElement(plBuilder, dslBuilder, reflectionData->getGlobalParamsTypeLayout());
		finishBuildingDescriptorSetLayout(plBuilder, dslBuilder);
		_completePLB = std::move(plBuilder);
	}

	void ShaderTransformer::addRangesForParameterBlockElement(PipelineLayoutBuilder& plBuilder,
															  DescriptorSetLayoutBuilder& dslBuilder,
															  slang::TypeLayoutReflection* elementTypeLayout) {
		if (elementTypeLayout->getSize() > 0) {
			addAutomaticallyIntroducedUniformBuffer(dslBuilder, elementTypeLayout);
		}
		addRanges(plBuilder, dslBuilder, elementTypeLayout);
	}

	// when the parameter block only contains ordinary data (no opaque items)
	void ShaderTransformer::addAutomaticallyIntroducedUniformBuffer(DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* elementTypeLayout) {
		auto vulkanBindingIndex = dslBuilder.getBindingCount();

		dslBuilder.addBinding(vulkanBindingIndex, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	}

	void ShaderTransformer::addRanges(PipelineLayoutBuilder& plBuilder, DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout) {
		addDescriptorRanges(dslBuilder, typeLayout);
		addSubRanges(plBuilder, typeLayout);
	}

	void ShaderTransformer::addDescriptorRanges(DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout) {
		int relativeSetIndex = 0;
		int rangeCount = typeLayout->getDescriptorSetDescriptorRangeCount(relativeSetIndex);
		for (int rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
			addDescriptorRange(dslBuilder, typeLayout, relativeSetIndex, rangeIndex);
		}
	}
	void ShaderTransformer::addDescriptorRange(DescriptorSetLayoutBuilder& dslBuilder, slang::TypeLayoutReflection* typeLayout, int relativeSetIndex, int rangeIndex) {
		slang::BindingType bindingType = typeLayout->getDescriptorSetDescriptorRangeType(relativeSetIndex, rangeIndex);
		uint32_t descriptorCount = typeLayout->getDescriptorSetDescriptorRangeDescriptorCount(relativeSetIndex, rangeIndex);

		// avoid push constants, they cant fit in descriptors
		if (bindingType == slang::BindingType::PushConstant) return;
		auto bindingIndex = dslBuilder.getBindingCount();
		dslBuilder.addBinding(bindingIndex, mapToDescriptorType(bindingType), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, descriptorCount);
	}

	void ShaderTransformer::addSubRanges(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* typeLayout) {
		int subRangeCount = typeLayout->getSubObjectRangeCount();
		for (int subRangeIndex = 0; subRangeIndex < subRangeCount; ++subRangeIndex) {
			addSubRange(plBuilder, typeLayout, subRangeIndex);
		}
	}
	void ShaderTransformer::addSubRange(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* typeLayout, int subRangeIndex) {
		int bindingRangeIndex = typeLayout->getSubObjectRangeBindingRangeIndex(subRangeIndex);
		slang::BindingType bindingType = typeLayout->getBindingRangeType(bindingRangeIndex);

		switch (bindingType) {
			default: return;
			case slang::BindingType::ParameterBlock: {
				auto parameterBlockTypeLayout = typeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);
				auto parameterBlockVariableLayout = typeLayout->getFieldByIndex(bindingRangeIndex);

				auto elementTypeLayout = parameterBlockTypeLayout->getElementTypeLayout();

				Shader::ParameterBlockInfo bindingInfo;
				bindingInfo.varName = parameterBlockVariableLayout->getName();
				bindingInfo.typeName = elementTypeLayout->getName();

				Slang::ComPtr<slang::IBlob> nameBlob;
				elementTypeLayout->getType()->getFullName(nameBlob.writeRef());
				bindingInfo.completeSlangName = static_cast<const char*>(nameBlob->getBufferPointer());

				bindingInfo.descriptorCount = 1;
				bindingInfo.usedStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
				bindingInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

				collectFieldsRecursively(bindingInfo.fields, elementTypeLayout);


				addDescriptorSetForParameterBlock(plBuilder, parameterBlockTypeLayout);
				const auto dslBuilder = plBuilder._dslBuilders.back();
				auto vulkanBindingIndex = dslBuilder.getBindingCount()-1;
				bindingInfo.bindingIndex = vulkanBindingIndex;
				bindingInfo.setIndex = dslBuilder.setIndex;
				_currentShader->_parameterBlocks.push_back(bindingInfo);
			} break;
			case slang::BindingType::PushConstant: {
				auto constantBufferTypeLayout = typeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);
				auto constantBufferVariableLayout = typeLayout->getFieldByIndex(bindingRangeIndex);

				auto elementTypeLayout = constantBufferTypeLayout->getElementTypeLayout();

				Shader::PushConstantInfo info;
				info.varName = constantBufferVariableLayout->getName();
				info.typeName = constantBufferTypeLayout->getName();
				Slang::ComPtr<slang::IBlob> nameBlob;
				constantBufferTypeLayout->getType()->getFullName(nameBlob.writeRef());
				info.completeSlangName = static_cast<const char*>(nameBlob->getBufferPointer());
				info.usedStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
				info.offset = 0;
				info.size = constantBufferTypeLayout->getElementTypeLayout()->getSize();
				// recurse into struct
				collectFieldsRecursively(info.fields, elementTypeLayout);

				_currentShader->_pushConstants.push_back(info);

				addPushConstantRangeForConstantBuffer(plBuilder, constantBufferTypeLayout);
			} break;
		}
	}

	void ShaderTransformer::addDescriptorSetForParameterBlock(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* paramBlockTypeLayout) {
		DescriptorSetLayoutBuilder newDSBuilder;
		startBuildingDescriptorSetLayout(plBuilder, newDSBuilder);
		addRangesForParameterBlockElement(plBuilder, newDSBuilder, paramBlockTypeLayout->getElementTypeLayout());
		finishBuildingDescriptorSetLayout(plBuilder, newDSBuilder);
	}

	void ShaderTransformer::addPushConstantRangeForConstantBuffer(PipelineLayoutBuilder& plBuilder, slang::TypeLayoutReflection* pushConstantBufferTypeLayout) {
		auto elementTypeLayout = pushConstantBufferTypeLayout->getElementTypeLayout();
		uint32_t elementSize = elementTypeLayout->getSize();

		if (elementSize <= 0) return;

		VkPushConstantRange range = {
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = elementSize
		};
		plBuilder.addPushConstantRange(range);
	}

	std::vector<VkDescriptorSetLayout> ShaderTransformer::retrieveDescriptorSetLayouts() {
		return _completePLB._builtDSLs;
	}
	VkPipelineLayout ShaderTransformer::retrievePipelineLayout(VkDevice device) {
		return _completePLB.build(device);
	}


}