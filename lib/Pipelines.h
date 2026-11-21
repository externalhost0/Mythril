//
// Created by Hayden Rivas on 10/11/25.
//

#pragma once

#include "mythril/Constants.h"
#include "mythril/Shader.h"
#include "mythril/PipelineSpecs.h"

#include <volk.h>

#include <variant>
#include <vector>


namespace mythril {
	class CTX;
	class CommandBuffer;

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

	class AllocatedRayTracingPipeline {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _shared.debugName; }

	private:
		VkStridedDeviceAddressRegionKHR _sbtEntryRayGen = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryMiss = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryHit = {};
		VkStridedDeviceAddressRegionKHR _sbtEntryCallable = {};
		Buffer _sbtBuffer;

		RayTracingPipelineSpec _spec;
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
		// dynamic rendering requires these to match the scope the pipeline draws in
		std::vector<VkFormat> _builtColorFormats;
		VkFormat _builtDepthFormat = VK_FORMAT_UNDEFINED;
		uint32_t _builtViewMask = 0;
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
