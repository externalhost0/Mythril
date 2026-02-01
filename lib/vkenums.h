//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include <volk.h>
namespace mythril {
	// ENUMS //
	enum class BlendingMode {
		OFF,
		ADDITIVE, // (glow)
		ALPHA_BLEND, // (standard)
		MULTIPLY,
		MASK // (stencil)
	};
	enum class DepthMode {
		NONE,
		LESS, // (standard)
		GREATER,
		EQUAL,
		ALWAYS
	};
	enum class CullMode {
		OFF,
		BACK, // (standard)
		FRONT
	};
	enum class PolygonMode {
		FILL, // (standard)
		LINE
	};
	enum class TopologyMode {
		TRIANGLE, // (standard)
		LIST,
		STRIP
	};
	enum class SampleCount : uint8_t {
		X1 = 0,
		X2,
		X4,
		X8
	};
	enum class SamplerFilter : uint8_t {
		Nearest,
		Linear
	};
	enum class SamplerMipMap : uint8_t {
		Disabled,
		Nearest,
		Linear
	};
	enum class SamplerWrap : uint8_t {
		Repeat,
		MirrorRepeat,
		ClampEdge,
		ClampBorder,
		MirrorClampEdge
	};
	enum class CompareOp : uint8_t {
		Never,
		Less,
		LessEqual,
		Equal,
		NotEqual,
		Greater,
		GreaterEqual,
		Always
	};
	enum class LoadOperation : uint8_t {
		NONE = 0,
		NO_CARE,
		CLEAR,
		LOAD
	};
	enum class StoreOperation : uint8_t {
		NONE = 0,
		NO_CARE,
		STORE,
	};
	enum class ResolveMode : uint8_t {
		AVERAGE = 0,
		MIN,
		MAX,
		SAMPLE_ZERO
	};
	enum class ShaderStages {
		Vertex,
		TesselationControl,
		TesselationEvaluation,
		Geometry,
		Fragment,
		Compute,
	};


	// FUNCTIONS //
	// our own to vulkan helpers


	// FIXME change how we handle multisampled images
	constexpr VkAttachmentStoreOp toVulkan(StoreOperation op) {
		switch (op) {
			case StoreOperation::NONE: return VK_ATTACHMENT_STORE_OP_NONE;
			case StoreOperation::NO_CARE: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
			case StoreOperation::STORE: return VK_ATTACHMENT_STORE_OP_STORE;
		}
	}
	constexpr VkAttachmentLoadOp toVulkan(LoadOperation op) {
		switch (op) {
			case LoadOperation::NONE: return VK_ATTACHMENT_LOAD_OP_NONE;
			case LoadOperation::NO_CARE: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			case LoadOperation::CLEAR: return VK_ATTACHMENT_LOAD_OP_CLEAR;
			case LoadOperation::LOAD: return VK_ATTACHMENT_LOAD_OP_LOAD;
		}
	}
	constexpr VkResolveModeFlagBits toVulkan(ResolveMode mode) {
		switch (mode) {
			case ResolveMode::MIN: return VK_RESOLVE_MODE_MIN_BIT;
			case ResolveMode::MAX: return VK_RESOLVE_MODE_MAX_BIT;
			case ResolveMode::AVERAGE: return VK_RESOLVE_MODE_AVERAGE_BIT;
			case ResolveMode::SAMPLE_ZERO: return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
		}
	}
	constexpr VkCullModeFlagBits toVulkan(CullMode mode) {
		switch (mode) {
			case CullMode::OFF: return VK_CULL_MODE_NONE;
			case CullMode::BACK: return VK_CULL_MODE_BACK_BIT;
			case CullMode::FRONT: return VK_CULL_MODE_FRONT_BIT;
		}
	}
	constexpr VkPolygonMode toVulkan(PolygonMode mode) {
		switch (mode) {
			case PolygonMode::FILL: return VK_POLYGON_MODE_FILL;
			case PolygonMode::LINE: return VK_POLYGON_MODE_LINE;
		}
	}
	constexpr VkPrimitiveTopology toVulkan(TopologyMode mode) {
		switch (mode) {
			case TopologyMode::TRIANGLE: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case TopologyMode::LIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case TopologyMode::STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		}
	}
	constexpr VkSampleCountFlagBits toVulkan(SampleCount count) {
		switch (count) {
			case SampleCount::X1: return VK_SAMPLE_COUNT_1_BIT;
			case SampleCount::X2: return VK_SAMPLE_COUNT_2_BIT;
			case SampleCount::X4: return VK_SAMPLE_COUNT_4_BIT;
			case SampleCount::X8: return VK_SAMPLE_COUNT_8_BIT;
		}
	}
	constexpr VkFilter toVulkan(SamplerFilter filter) {
		switch (filter) {
			case SamplerFilter::Nearest: return VK_FILTER_NEAREST;
			case SamplerFilter::Linear:  return VK_FILTER_LINEAR;
		}
	}
	constexpr VkSamplerAddressMode toVulkan(SamplerWrap wrap) {
		switch (wrap) {
			case SamplerWrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case SamplerWrap::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case SamplerWrap::ClampEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case SamplerWrap::ClampBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			case SamplerWrap::MirrorClampEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		}
	}
	constexpr VkSamplerMipmapMode toVulkan(SamplerMipMap mip) {
		switch (mip) {
			case SamplerMipMap::Disabled:
			case SamplerMipMap::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
			case SamplerMipMap::Linear: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}
	}
	constexpr VkCompareOp toVulkan(CompareOp op) {
		switch (op) {
			case CompareOp::Never: return VK_COMPARE_OP_NEVER;
			case CompareOp::Less: return VK_COMPARE_OP_LESS;
			case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
			case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
			case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
			case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
		}
	}
	constexpr VkShaderStageFlagBits toVulkan(ShaderStages stage) {
		switch (stage) {
			case ShaderStages::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStages::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStages::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
			case ShaderStages::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
			case ShaderStages::TesselationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			case ShaderStages::TesselationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		}
	}

}
