//
// Created by Hayden Rivas on 11/1/25.
//

#include "Shader.h"
#include "HelperMacros.h"
#include "Logger.h"

#include <iostream>
#include <map>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>
#include <spirv_reflect.h>


namespace mythril {
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
			case SLANG_TEXTURE_COMBINED_FLAG:        return "SLANG_TEXTURE_COMBINED_FLAG";
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
			case slang::TypeReflection::Kind::MeshOutput:           return "MeshOutput";
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
			case SLANG_STAGE_DISPATCH:       return "Dispatch";
		}
	}
	VkShaderStageFlags SlangStageToVkShaderStage(SlangStage stage) {
		switch (stage) {
			case SLANG_STAGE_NONE: return 0;
			case SLANG_STAGE_VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
			case SLANG_STAGE_HULL: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			case SLANG_STAGE_DOMAIN: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			case SLANG_STAGE_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
			case SLANG_STAGE_FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case SLANG_STAGE_COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
			case SLANG_STAGE_RAY_GENERATION: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
			case SLANG_STAGE_INTERSECTION: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
			case SLANG_STAGE_ANY_HIT: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
			case SLANG_STAGE_CLOSEST_HIT: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
			case SLANG_STAGE_MISS: return VK_SHADER_STAGE_MISS_BIT_KHR;
			case SLANG_STAGE_CALLABLE: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
			case SLANG_STAGE_MESH: return VK_SHADER_STAGE_MESH_BIT_EXT;
			case SLANG_STAGE_AMPLIFICATION: return VK_SHADER_STAGE_TASK_BIT_EXT;

			default: return 0;
		}
	}
#include <vulkan/vulkan.h>

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
				return "VK_DESCRIPTOR_TYPE_UNKNOWN";
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
				return FieldKind::Scalar;
			default:
				return FieldKind::Unknown;
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
			case FieldKind::Unknown:      return "Unknown";
			case FieldKind::Pointer:      return "Pointer";
		}
	}

	const char* SpvOpToString(SpvOp op) {
		switch (op) {
#define OP(X) case X: return #X
			OP(SpvOpNop);
			OP(SpvOpUndef);
			OP(SpvOpSourceContinued);
			OP(SpvOpSource);
			OP(SpvOpSourceExtension);
			OP(SpvOpName);
			OP(SpvOpMemberName);
			OP(SpvOpString);
			OP(SpvOpLine);
			OP(SpvOpExtension);
			OP(SpvOpExtInstImport);
			OP(SpvOpExtInst);
			OP(SpvOpMemoryModel);
			OP(SpvOpEntryPoint);
			OP(SpvOpExecutionMode);
			OP(SpvOpCapability);
			OP(SpvOpTypeVoid);
			OP(SpvOpTypeBool);
			OP(SpvOpTypeInt);
			OP(SpvOpTypeFloat);
			OP(SpvOpTypeVector);
			OP(SpvOpTypeMatrix);
			OP(SpvOpTypeImage);
			OP(SpvOpTypeSampler);
			OP(SpvOpTypeSampledImage);
			OP(SpvOpTypeArray);
			OP(SpvOpTypeRuntimeArray);
			OP(SpvOpTypeStruct);
			OP(SpvOpTypeOpaque);
			OP(SpvOpTypePointer);
			OP(SpvOpTypeFunction);
			OP(SpvOpTypeEvent);
			OP(SpvOpTypeDeviceEvent);
			OP(SpvOpTypeReserveId);
			OP(SpvOpTypeQueue);
			OP(SpvOpTypePipe);
			OP(SpvOpTypeForwardPointer);
				// ... add more here as needed ...
#undef OP
			default: return "<Unknown SpvOp>";
		}
	}

#include <string>
#include <optional>
#include <cctype>

	struct MatrixDims {
		int rows;
		int cols;
	};

	std::optional<MatrixDims> ExtractMatrixDims(const std::string& s) {
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

	bool DetectMatrixType(const SpvReflectTypeDescription& typeDesc) {
		if (typeDesc.struct_type_description) {
			std::string s = typeDesc.type_name;
			return s.find("MatrixStorage") != std::string::npos;
		}
		return false;
	}
	FieldKind ResolveKind(const SpvReflectTypeDescription& typeDesc) {
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

	ScalarKind ResolveScalarKindFromType(const SpvReflectTypeDescription& typeDesc) {
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
			case SpvOpTypeBool: {
				return ScalarKind::Bool;
			}
			default: break;
			ASSERT_MSG(false, "Something went horribly wrong, this should never happen!");
		}
	}

	void CollectFieldsRecursively(std::vector<FieldInfo>& fields, const SpvReflectBlockVariable& blockVar) {
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


	void GatherDescriptorBindingInformation(Shader::DescriptorBindingInfo& info, const SpvReflectDescriptorBinding& descriptorBind) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *descriptorBind.type_description;
		// descriptor info
		info.descriptorType = static_cast<VkDescriptorType>(descriptorBind.descriptor_type);
		info.descriptorCount = descriptorBind.count;
		info.setIndex = descriptorBind.set;
		info.bindingIndex = descriptorBind.binding;
		// top level info
		info.typeName = blockType.type_name;
		info.varName = descriptorBind.name;

		CollectFieldsRecursively(info.fields, descriptorBind.block);
	}
	void GatherDescriptorSetInformation(Shader::DescriptorSetInfo& info, const SpvReflectDescriptorSet& descriptorSet) {
		info.setIndex = descriptorSet.set;
		info.bindingInfos.reserve(descriptorSet.binding_count);
		for (int i = 0; i < descriptorSet.binding_count; i++) {
			Shader::DescriptorBindingInfo binding_info = {};
			GatherDescriptorBindingInformation(binding_info, *descriptorSet.bindings[i]);
			info.bindingInfos.push_back(binding_info);
		}
	}

	void GatherPushConstantInformation(Shader::PushConstantInfo& info, const SpvReflectBlockVariable& blockVar) {
		// alias top level type
		const SpvReflectTypeDescription& blockType = *blockVar.type_description;
		// top level info
		info.size = blockVar.size;
		info.offset = blockVar.offset;
		info.varName = blockVar.name;
		info.typeName = blockType.type_name;

		CollectFieldsRecursively(info.fields, blockVar);
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

		uint32_t ds_count = 0;
		spv_result = spvReflectEnumerateDescriptorSets(&spirv_module, &ds_count, nullptr);
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for count.");

		std::vector<SpvReflectDescriptorSet*> sets(ds_count);
		spv_result = spvReflectEnumerateDescriptorSets(&spirv_module, &ds_count, sets.data());
		ASSERT_MSG(spv_result == SPV_REFLECT_RESULT_SUCCESS, "Failed to enumerate Descriptor Sets for data.");

		// begin collecting information for the result here
		ReflectionResult result = {};
		result.pipelineLayoutSignature.setSignatures.resize(sets.size());
		result.retrievedDescriptorSets.reserve(sets.size()); // this reserves +1 when bindless exists

		// collect descriptor sets info
		// lets make every shader own its descriptor sets even if they are duplicated, optimize later with a pool of a sort whe signatures match
		for (unsigned int i_set = 0; i_set < sets.size(); i_set++) {
			// get a set from the array we enumerated
			const SpvReflectDescriptorSet* reflected_set = sets[i_set];
			// modify the set signature in place
			DescriptorSetSignature& set_signature = result.pipelineLayoutSignature.setSignatures[i_set];
			set_signature.isBindless = false;
			set_signature.setIndex = reflected_set->set;
			set_signature.bindings.reserve(reflected_set->binding_count);

			// for bonus reflection, as done in push constants aswell
			// filled in later by each binding, must be done as we move across bindings in order to avoid bindless descriptor
			Shader::DescriptorSetInfo set_info = {};
			set_info.setIndex = reflected_set->set;

			// now move through all the descriptor bindings in the set
			for (unsigned int j_binding = 0; j_binding < reflected_set->binding_count; j_binding++) {
				// get a binding from the set
				const SpvReflectDescriptorBinding* reflected_binding = reflected_set->bindings[j_binding];
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
				// fixme, correctly get stageflags, this is also done for push constants
				vkds_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
				vkds_layout_binding.pImmutableSamplers = nullptr;
				// thats all the info for a binding
				// this is for lookup when updating
				set_signature.nameToBinding[reflected_binding->name] = reflected_binding->binding;

				// bonus information for user
				Shader::DescriptorBindingInfo binding_info = {};
				GatherDescriptorBindingInformation(binding_info, *reflected_binding);
				set_info.bindingInfos.push_back(binding_info);

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
			pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			pc_ranges[i_block] = pc_range;

			Shader::PushConstantInfo pushInfo = {};
			GatherPushConstantInformation(pushInfo, *block);
			result.retrivedPushConstants.push_back(std::move(pushInfo));
		}
		result.pipelineLayoutSignature.pushes = pc_ranges;

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