//
// Created by Hayden Rivas on 1/16/25.
//
#pragma once

#include "faststl/StackVector.h"
#include "vkenums.h"

#include <span>
#include <vector>
#include <volk.h>

namespace mythril {
	class GraphicsPipelineBuilder {
	public:
		GraphicsPipelineBuilder() { Clear(); };
		~GraphicsPipelineBuilder() = default;
	public:
		VkPipelineInputAssemblyStateCreateInfo _inputAssembly = {};
		VkPipelineRasterizationStateCreateInfo _rasterizer = {};
		VkPipelineMultisampleStateCreateInfo _multisampling = {};
		VkPipelineDepthStencilStateCreateInfo _depthStencil = {};
		VkPipelineRenderingCreateInfo _renderInfo = {};
		VkPipelineColorBlendAttachmentState _colorBlendAttachment = {};

		// max of 4 stages per pipeline
		StackVector<VkPipelineShaderStageCreateInfo, 4> _shaderStages = {};
		StackVector<VkFormat, 12> _colorAttachmentFormats = {};
	public:
		VkPipeline build(VkDevice device, VkPipelineLayout layout);
		void Clear();
	public:
		// program
		GraphicsPipelineBuilder& add_shader_module(const VkShaderModule& module, VkShaderStageFlags stageFlags, const char* entryPoint, VkSpecializationInfo* spInfo = nullptr);

		// props
		GraphicsPipelineBuilder& set_topology_mode(TopologyMode mode);
		GraphicsPipelineBuilder& set_polygon_mode(PolygonMode mode);
		GraphicsPipelineBuilder& set_multisampling_mode(SampleCount count);
		GraphicsPipelineBuilder& set_cull_mode(CullMode mode);
		GraphicsPipelineBuilder& set_blending_mode(BlendingMode mode);
		GraphicsPipelineBuilder& set_viewmask(uint32_t bitmask);

		// formats
		GraphicsPipelineBuilder& set_color_formats(std::span<VkFormat> formats);
		GraphicsPipelineBuilder& set_depth_format(VkFormat format);
	private:
		void _setBlendtoOff();
		void _setBlendtoAlphaBlend();
		void _setBlendtoAdditive(); // ext
		void _setBlendtoMultiply(); // ext
		void _setBlendtoPremultiplied(); // ext
		void _setBlendtoMask(); // ext
	};
}