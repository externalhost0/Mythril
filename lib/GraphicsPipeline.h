//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <slang/slang.h>

namespace mythril {
	enum class ShaderStages {
		Vertex,
		TesselationControl,
		TesselationEvaluation,
		Geometry,
		Fragment,
		Compute,
	};
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

	struct ShaderStage {
		InternalShaderHandle handle {};
		const char* entryPoint = nullptr;

		ShaderStage() = default;
		ShaderStage(InternalShaderHandle handle) : handle(handle) {}
		ShaderStage(InternalShaderHandle handle, const char* entryPoint) : handle(handle), entryPoint(entryPoint) {}

		bool valid() const noexcept {
			return handle.valid();
		}
	};
	struct GraphicsPipelineSpec {
		ShaderStage vertexShader;
		ShaderStage fragmentShader;
		ShaderStage geometryShader;

		TopologyMode topology = TopologyMode::TRIANGLE;
		PolygonMode polygon = PolygonMode::FILL;
		BlendingMode blend = BlendingMode::OFF;
		CullMode cull = CullMode::BACK;
		SampleCount multisample = SampleCount::X1;
		const char* debugName = "Unnamed Graphics Pipeline";
	};
	struct RayTracingPipelineSpec {

		const char* debugName = "Unnamed RayTracing Pipeline";
	};
	struct ComputePipelineSpec {
		InternalShaderHandle handle;
		const char* entryPoint;
		const char* debugName = "Unnamed Compute Pipeline";
	};

	enum class PipelineType {
		Graphics,
		Compute,
		RayTracing
	};

	struct IPipeline {
		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;
		PipelineType _type = PipelineType::Graphics;

		char _debugName[128] = {0};
	};
	class ComputePipeline : public IPipeline {

	private:
		friend class CTX;
		friend class CommandBuffer;

	};
	class RayTracingPipeline : IPipeline {

	private:
		friend class CTX;
		friend class CommandBuffer;
	};

	class GraphicsPipeline : public IPipeline {
	public:

	private:
		GraphicsPipelineSpec _spec;
		PipelineLayoutSignature signature;

		struct ManagedDescriptorSet {
			VkDescriptorSet vkDescriptorSet = VK_NULL_HANDLE;
			VkDescriptorSetLayout vkDescriptorSetLayout = VK_NULL_HANDLE;
		};
		std::vector<ManagedDescriptorSet> _managedDescriptorSets;
		std::vector<VkDescriptorSet> _vkBindableDescriptorSets;

		friend class CTX;
		friend class CommandBuffer;
		friend class DescriptorSetWriter;
	};

}