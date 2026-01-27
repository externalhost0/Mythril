//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include "vkenums.h"
#include "Constants.h"
#include "Shader.h"

#include <volk.h>
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
		ShaderHandle handle{};
		const char* entryPoint = nullptr;

		ShaderStage() = default;
		ShaderStage(const Shader& shader);
		ShaderStage(ShaderHandle handle) : handle(handle) {}
		ShaderStage(ShaderHandle handle, const char* entryPoint) : handle(handle), entryPoint(entryPoint) {}

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
		const void* data = nullptr;
		size_t size = 0;
		std::variant<std::string, int> identifier;
	};

	struct GraphicsPipelineSpec {
		ShaderStage vertexShader;
		ShaderStage fragmentShader;
		// ShaderStage geometryShader;

		TopologyMode topology = TopologyMode::TRIANGLE;
		PolygonMode polygon = PolygonMode::FILL;
		BlendingMode blend = BlendingMode::OFF;
		CullMode cull = CullMode::OFF;
		// todo: easily make multisample resolveable during compile and remove this field
		SampleCount multisample = SampleCount::X1;
		// max spec constants of 16
		SpecializationConstantEntry specConstants[16];
		const char* debugName = "Unnamed Graphics Pipeline";
	};

	struct ComputePipelineSpec {
		ShaderHandle shader;
		SpecializationConstantEntry specConstants[16];
		const char* debugName = "Unnamed Compute Pipeline";
	};
	struct RayTracingPipelineSpec {
		ShaderHandle shader;
		SpecializationConstantEntry specConstants[16];
		const char* debugName = "Unnamed RayTracing Pipeline";
	};


	// information that any type of pipeline also has
	struct PipelineCoreData {
		// information all pipelines have anyways
		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;

		// used upon resolving to build vkPipelineLayout
		PipelineLayoutSignature signature;
		// a managed descriptor set stores its layout per lifetime necessities
		struct ManagedDescriptorSet {
			VkDescriptorSet vkDescriptorSet = VK_NULL_HANDLE;
			VkDescriptorSetLayout vkDescriptorSetLayout = VK_NULL_HANDLE;
		};
		// used for allocation and destruction
		std::vector<ManagedDescriptorSet> _managedDescriptorSets;

		// used for binding in cmdBind*Pipeline
		// should only serve as refrences to descriptor sets, not tracking
		std::vector<VkDescriptorSet> _vkBindableDescriptorSets;
	};

	enum class PipelineType { Graphics, Compute, RayTracing };

	struct SharedPipelineInfo {
		PipelineCoreData core;
		char debugName[kMaxDebugNameLength] = {0};
	};

	class AllocatedRayTracingPipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }
	private:
		RayTracingPipelineSpec _spec;
		SharedPipelineInfo _shared;


		friend class CTX;
		friend class CommandBuffer;
	};

	class AllocatedGraphicsPipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }
	private:
		GraphicsPipelineSpec _spec;
		SharedPipelineInfo _shared;


		friend class CTX;
		friend class CommandBuffer;
		friend class DescriptorSetWriter;
	};

	class AllocatedComputePipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }
	private:
		ComputePipelineSpec _spec;
		SharedPipelineInfo _shared;


		friend class CTX;
		friend class CommandBuffer;

	};

}
