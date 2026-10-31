//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include "Constants.h"
#include "Shader.h"
#include "mythril/vkenums.h"

#include <slang/slang.h>
#include <volk.h>

#include <variant>

#include "mythril/Objects.h"

namespace mythril {
	class CTX;
	class CommandBuffer;

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
		// recently added
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
	struct RaytracingPipelineSpec {
		std::vector<ShaderStage> rayGenShaders;
		std::vector<ShaderStage> missShaders;
		std::vector<ShaderStage> closestHitShaders;
		std::vector<ShaderStage> callableShader;
		SpecializationConstantEntry specConstants[16];
		uint32_t maxRecursionDepth = 0;
		uint32_t stackSize = 0;
		const char* debugName = "Unnamed RayTracing Pipeline";
	};


	namespace detail {

	// information that any type of pipeline also has
	struct PipelineCoreData {
		// information all pipelines have anyways
		VkPipeline _vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout _vkPipelineLayout = VK_NULL_HANDLE;

		// used upon resolving to build vkPipelineLayout
		PipelineLayoutSignature signature;

		// our managed descriptor set the pipeline can bind to, if its shader uses it
		// VK_NULL_HANDLE means the pipeline uses no descriptor sets at all (can still use push-constants)
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

	class AllocatedRaytracingPipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }

	private:
		VkStridedDeviceAddressRegionKHR _sbtEntryRayGen = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryMiss = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryHit = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryCallable = {};
		Buffer _sbtBuffer;

		RaytracingPipelineSpec _spec;
		SharedPipelineInfo _shared;
		friend class ::mythril::CTX;
		friend class ::mythril::CommandBuffer;
	};

	class AllocatedGraphicsPipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }

	private:
		GraphicsPipelineSpec _spec;
		SharedPipelineInfo _shared;
		friend class ::mythril::CTX;
		friend class ::mythril::CommandBuffer;
	};

	class AllocatedComputePipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }

	private:
		ComputePipelineSpec _spec;
		SharedPipelineInfo _shared;
		friend class ::mythril::CTX;
		friend class ::mythril::CommandBuffer;
	};

	} // namespace detail
} // namespace mythril
