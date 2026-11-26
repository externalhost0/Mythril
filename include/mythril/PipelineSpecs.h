//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include "Objects.h"
#include "vkenums.h"

#include <string>
#include <variant>
#include <vector>

namespace mythril {
	// use ShaderStage as the parameters since it means we can optionaly specify the entrypoint
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
		ShaderStage(const Shader& shader, const char* entryPoint);

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
		std::vector<ShaderStage> rayGenShaders;
		std::vector<ShaderStage> missShaders;
		std::vector<ShaderStage> closestHitShaders;
		std::vector<ShaderStage> callableShader;
		SpecializationConstantEntry specConstants[16];
		uint32_t maxRecursionDepth = 0;
		uint32_t stackSize = 0;
		const char* debugName = "Unnamed RayTracing Pipeline";
	};
} // namespace mythril
