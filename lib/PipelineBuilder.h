//
// Created by Hayden Rivas on 1/16/25.
//
#pragma once

#include "faststl/FastVector.h"
#include "vkenums.h"

#include <span>
#include <vector>
#include <volk.h>

namespace mythril {
	class PipelineBuilder {
	public:
		PipelineBuilder() { Clear(); };
		~PipelineBuilder() = default;
	public:
		VkPipelineInputAssemblyStateCreateInfo _inputAssembly = {};
		VkPipelineRasterizationStateCreateInfo _rasterizer = {};
		VkPipelineMultisampleStateCreateInfo _multisampling = {};
		VkPipelineDepthStencilStateCreateInfo _depthStencil = {};
		VkPipelineRenderingCreateInfo _renderInfo = {};
		VkPipelineColorBlendAttachmentState _colorBlendAttachment = {};

		// max of 4 stages per pipeline
		FastVector<VkPipelineShaderStageCreateInfo, 4> _shaderStages = {};
		FastVector<VkFormat, 12> _colorAttachmentFormats = {};
	public:
		VkPipeline build(VkDevice device, VkPipelineLayout layout);
		void Clear();
	public:
		// program
		PipelineBuilder& add_shader_module(const VkShaderModule& module, VkShaderStageFlags stageFlags, const char* entryPoint, VkSpecializationInfo* spInfo = nullptr);

		// props
		PipelineBuilder& set_topology_mode(TopologyMode mode);
		PipelineBuilder& set_polygon_mode(PolygonMode mode);
		PipelineBuilder& set_multisampling_mode(SampleCount count);
		PipelineBuilder& set_cull_mode(CullMode mode);
		PipelineBuilder& set_blending_mode(BlendingMode mode);

		// formats
		PipelineBuilder& set_color_formats(std::span<VkFormat> formats);
		PipelineBuilder& set_depth_format(VkFormat format);
	private:
		void _setBlendtoOff();
		void _setBlendtoAlphaBlend();
		void _setBlendtoAdditive(); // ext
		void _setBlendtoMultiply(); // ext
		void _setBlendtoPremultiplied(); // ext
		void _setBlendtoMask(); // ext
	};
}