//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <slang/slang.h>

namespace mythril {
	struct ShaderParameter {
		struct LayoutUnits {
			size_t bytes = 0;
			uint32_t descriptorSets = 0;
			uint32_t descriptorBinds = 0;
		};
		std::string name;
		std::string typeName;
		std::string completeName;

		uint32_t set;
		uint32_t binding;
		LayoutUnits size;

		// kind is what the type is supposed to be in the shader
		slang::TypeReflection::Kind kind;
		// category is how vulkan consumes the paramater
		slang::ParameterCategory category;

		VkDescriptorType descriptorType;
		VkShaderStageFlags stages;

		bool isBindless; // if arary and has size of 0
	};

	enum class ShaderStages {
		Vertex,
		Fragment,
		Geometry,
		Compute
	};
	constexpr VkShaderStageFlagBits toVulkan(ShaderStages stage) {
		switch (stage) {
			case ShaderStages::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStages::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStages::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
			case ShaderStages::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
		}
	}

	struct PipelineSpec {
		struct ShaderStage {
			InternalShaderHandle handle;
			const char* entryPoint;
			ShaderStages stage;
		};
		std::vector<ShaderStage> stages = {};

		TopologyMode topology = TopologyMode::TRIANGLE;
		PolygonMode polygon = PolygonMode::FILL;
		BlendingMode blend = BlendingMode::OFF;
		CullMode cull = CullMode::BACK;
		SampleCount multisample = SampleCount::X1;
		const char* debugName = "Unnamed Pipeline";
	};

	enum class PipelineType {
		Graphics,
		Compute,
		RayTracing
	};
	class IPipeline {
		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;
		PipelineType _type = PipelineType::Graphics;
	};
	class ComputePipeline : IPipeline {

	};
	class RayTracingPipeline : IPipeline {

	};
	class GraphicsPipeline : IPipeline {
	private:
		PipelineSpec _spec;

		std::vector<VkDescriptorSet> _nonbindlessDescriptorSets = {};

		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout _vkLastDescriptorSetLayout = VK_NULL_HANDLE;

		friend class CTX;
		friend class CommandBuffer;
	};

}