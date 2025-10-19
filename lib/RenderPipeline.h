//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

namespace mythril {
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
			InternalShaderHandle handle = {};
			const char* entryPoint = nullptr;
			ShaderStages stage;
		};
		std::vector<ShaderStage> stages = {};

		TopologyMode topology = TopologyMode::TRIANGLE;
		PolygonMode polygon = PolygonMode::FILL;
		BlendingMode blend = BlendingMode::OFF;
		CullMode cull = CullMode::OFF;
		SampleCount multisample = SampleCount::X1;
		const char* debugName = "Unnamed Pipeline";
	};
	class RenderPipeline {
	private:
		PipelineSpec _spec;

		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout _vkLastDescriptorSetLayout = VK_NULL_HANDLE;

		friend class CTX;
		friend class CommandBuffer;
	};

}