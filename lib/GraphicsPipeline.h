//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include <slang/slang.h>

#include <variant>

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


	struct SpecializationConstantEntry {
		SpecializationConstantEntry() = default;
		// explicit construction
		SpecializationConstantEntry(const void* data, size_t size, const std::variant<std::string, int>& id) {
			this->data = data;
			this->size = size;
			this->identifier = id;
		}
		// implicit construction (recommended)
//		template<typename T>
//		SpecializationConstantEntry(const T& type, const std::variant<std::string, int>& id) {
//			SpecializationConstantEntry(&type, sizeof(T), id);
//		}
		const void* data = nullptr;
		size_t size = 0;
		std::variant<std::string, int> identifier;
	};

	struct GraphicsPipelineSpec {
		ShaderStage vertexShader;
		ShaderStage fragmentShader;
		ShaderStage geometryShader;

		TopologyMode topology = TopologyMode::TRIANGLE;
		PolygonMode polygon = PolygonMode::FILL;
		BlendingMode blend = BlendingMode::OFF;
		CullMode cull = CullMode::OFF;
		SampleCount multisample = SampleCount::X1;

		// max spec constants of 16
		SpecializationConstantEntry specConstants[16];
		const char* debugName = "Unnamed Graphics Pipeline";
	};
	struct RayTracingPipelineSpec {
		InternalShaderHandle shader;
		const char* debugName = "Unnamed RayTracing Pipeline";
	};
	struct ComputePipelineSpec {
		InternalShaderHandle shader;
		SpecializationConstantEntry specConstants[16];
		const char* debugName = "Unnamed Compute Pipeline";
	};


	struct IPipeline {
		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;
		char _debugName[128] = {0};
	};
	class RayTracingPipeline : public IPipeline {

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

	class ComputePipeline : public IPipeline {
	public:

	private:


		friend class CTX;
		friend class CommandBuffer;

	};

}