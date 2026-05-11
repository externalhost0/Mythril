//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include "Constants.h"
#include "Shader.h"
#include "vkenums.h"

#include <slang/slang.h>
#include <volk.h>

#include <variant>

namespace mythril {


	struct ShaderStage {
		ShaderHandle handle{};
		const char* entryPoint = nullptr;

		ShaderStage() = default;
		ShaderStage(const Shader& shader);
		ShaderStage(ShaderHandle handle) :
		    handle(handle) {}
		ShaderStage(ShaderHandle handle, const char* entryPoint) :
		    handle(handle),
		    entryPoint(entryPoint) {}

		bool valid() const noexcept { return handle.valid(); }
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
		// todo: easily make determining if multisample during compile and remove this field
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

		// our managed descriptor set the pipeline can bind to, if its shader uses it
		// VK_NULL_HANDLE means the pipeline uses no descriptor sets at all (push-constants only).
		VkDescriptorSet _vkBindlessDescriptorSet = VK_NULL_HANDLE;
	};

	enum class PipelineType {
		Graphics,
		Compute,
		RayTracing
	};

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

} // namespace mythril
