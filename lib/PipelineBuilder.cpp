//
// Created by Hayden Rivas on 10/11/25.
//

#include "GraphicsPipelineBuilder.h"
#include "vkinfo.h"
#include "vkenums.h"
#include "HelperMacros.h"
#include "Logger.h"

#include <volk.h>
#include <algorithm>

#include "vkutil.h"

namespace mythril {
	// some helper functions //
	bool isIntegarFormat(VkFormat format) {
		return (format == VK_FORMAT_R8_UINT || format == VK_FORMAT_R16_UINT || format == VK_FORMAT_R32_UINT ||
				format == VK_FORMAT_R8G8_UINT || format == VK_FORMAT_R16G16_UINT || format == VK_FORMAT_R32G32_UINT ||
				format == VK_FORMAT_R8G8B8_UINT || format == VK_FORMAT_R16G16B16_UINT || format == VK_FORMAT_R32G32B32_UINT ||
				format == VK_FORMAT_R8G8B8A8_UINT || format == VK_FORMAT_R16G16B16A16_UINT || format == VK_FORMAT_R32G32B32A32_UINT);
	}

	void GraphicsPipelineBuilder::Clear() {
		// clear all of the structs we need back to 0 with their correct stype
		_inputAssembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		_rasterizer = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		_multisampling = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		_depthStencil = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		_renderInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };

		_colorBlendAttachment = {};
		// vector clears
		_colorAttachmentFormats.clear();
		_shaderStages.clear();
	}
	// build() needs to be the last function called on the builder!
	VkPipeline GraphicsPipelineBuilder::build(VkDevice device, VkPipelineLayout layout) {
		// the create info for the pipeline we are building
		VkGraphicsPipelineCreateInfo graphics_pipeline_ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = nullptr };

		// --- shaders --- //
		graphics_pipeline_ci.stageCount = static_cast<uint32_t>(_shaderStages.size());
		graphics_pipeline_ci.pStages = _shaderStages.data();

		// ---- colorblending ---- //
		VkPipelineColorBlendStateCreateInfo colorBlending = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .pNext = nullptr };
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		// copy the set blend mode into our attachments, we than do some conditional modifying
		std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(_colorAttachmentFormats.size(), _colorBlendAttachment);
		// if blending is enabled in one of the helper functions, this catches formats that cant use transparency and disables it
		// disable blending if format doesnt accept it
		for (int i = 0; i < blendAttachments.size(); i++) {
			if (isIntegarFormat(_colorAttachmentFormats[i])) {
				LOG_SYSTEM(LogType::Warning, "Color Attachment {} does not support blending and has blendEnabled to VK_FALSE", i);
				blendAttachments[i].blendEnable = VK_FALSE;
			}
		}
		colorBlending.attachmentCount = blendAttachments.size();
		colorBlending.pAttachments = blendAttachments.data();
		std::ranges::fill(colorBlending.blendConstants, 0.f);
		graphics_pipeline_ci.pColorBlendState = &colorBlending;

		// ----- dynamic states ----- //
		std::array<VkDynamicState, 8> states = {
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR,
				VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
				VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
				VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
				VK_DYNAMIC_STATE_DEPTH_BIAS,
				VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
				VK_DYNAMIC_STATE_BLEND_CONSTANTS
		};

		VkPipelineDynamicStateCreateInfo dynamicInfo = vkinfo::CreatePipelineDynamicStateInfo(states.data(), states.size());
		graphics_pipeline_ci.pDynamicState = &dynamicInfo;

		// ------ vertex input ig doesnt matter because we are using (push constants + device address) ----- //
		VkPipelineVertexInputStateCreateInfo _vertexInputInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nullptr };
		graphics_pipeline_ci.pVertexInputState = &_vertexInputInfo;

		// ------ viewport state ------ //
		VkPipelineViewportStateCreateInfo viewportState = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = nullptr };
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		graphics_pipeline_ci.pViewportState = &viewportState;

		// everything else that doesnt need configuring on build()
		graphics_pipeline_ci.pNext = &_renderInfo;
		graphics_pipeline_ci.pInputAssemblyState = &_inputAssembly;
		graphics_pipeline_ci.pRasterizationState = &_rasterizer;
		graphics_pipeline_ci.pMultisampleState = &_multisampling;
		graphics_pipeline_ci.pDepthStencilState = &_depthStencil; // for setncil operations which are not dynamic

		graphics_pipeline_ci.layout = layout; // just the user given layout

		// -- actual creation -- //
		VkPipeline newPipeline = VK_NULL_HANDLE;
		VK_CHECK(vkCreateGraphicsPipelines(device, nullptr, 1, &graphics_pipeline_ci, nullptr, &newPipeline));
		this->Clear(); // clear the entire pipeline struct to reuse the PipelineBuilder
		return newPipeline;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::add_shader_module(const VkShaderModule& module, VkShaderStageFlags stageFlags, const char* entryPoint, VkSpecializationInfo* spInfo) {
		VkPipelineShaderStageCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		info.pNext = nullptr;
		info.stage = static_cast<VkShaderStageFlagBits>(stageFlags);
		info.module = module;
		info.pName = entryPoint; // entry point default to "main"

		info.pSpecializationInfo = spInfo;

		_shaderStages.push_back(info);
		return *this;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_polygon_mode(PolygonMode polygonMode) {
		_rasterizer.polygonMode = toVulkan(polygonMode);
		_rasterizer.lineWidth = 1.0f; // we cant change width, metal doesnt support wide lines

		return *this;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_topology_mode(TopologyMode topoMode) {
		VkPrimitiveTopology topology = toVulkan(topoMode);
		_inputAssembly.topology = topology;
		_inputAssembly.primitiveRestartEnable = VK_TRUE;
		if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST 				||
			topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST 				||
			topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 			||
			topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY  ||
			topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY) _inputAssembly.primitiveRestartEnable = VK_FALSE;

		return *this;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_cull_mode(CullMode cullMode) {
		_rasterizer.cullMode = toVulkan(cullMode);
		_rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

		return *this;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_multisampling_mode(SampleCount count) {
		_multisampling.sampleShadingEnable = VK_FALSE;
		_multisampling.rasterizationSamples = toVulkan(count);
		_multisampling.minSampleShading = 1.0f;
		_multisampling.pSampleMask = nullptr;
		_multisampling.alphaToCoverageEnable = VK_FALSE;
		_multisampling.alphaToOneEnable = VK_FALSE;

		return *this;
	}
	// for multiple formats
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_color_formats(std::span<VkFormat> formats) {
		for (const VkFormat& format : formats) {
			_colorAttachmentFormats.push_back(format);
		}
		_renderInfo.colorAttachmentCount = formats.size();
		_renderInfo.pColorAttachmentFormats = formats.data();

		return *this;
	}

	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_depth_format(VkFormat format) {
		_renderInfo.depthAttachmentFormat = format;

		return *this;
	}

	// Off
	void GraphicsPipelineBuilder::_setBlendtoOff() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_FALSE;
	}
	// Normal
	void GraphicsPipelineBuilder::_setBlendtoAlphaBlend() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_TRUE;
		// source alpha is from alpha channel
		_colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

		_colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; // diff

		_colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		_colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		_colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; // diff
		_colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	// Additive
	void GraphicsPipelineBuilder::_setBlendtoAdditive() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_TRUE;
		// source alpha is from alpha channel
		_colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

		_colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // diff

		_colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		_colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		_colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		_colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	// Multiply
	void GraphicsPipelineBuilder::_setBlendtoMultiply() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_TRUE;
		// source alpha is from color
		_colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; // diff

		_colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // diff

		_colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		_colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		_colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		_colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	// Premultiplied
	void GraphicsPipelineBuilder::_setBlendtoPremultiplied() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_TRUE;
		// source alpha is constant 1
		_colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // diff

		_colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; // diff

		_colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		_colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		_colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; // diff
		_colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	// Stencil Mask
	void GraphicsPipelineBuilder::_setBlendtoMask() {
		_colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		_colorBlendAttachment.blendEnable = VK_TRUE;
		// source alpha is from alpha channel
		_colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

		_colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;

		_colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		_colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		_colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		_colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_blending_mode(BlendingMode mode) {
		switch (mode) {
			case BlendingMode::OFF:
				this->_setBlendtoOff();
				break;
			case BlendingMode::ADDITIVE:
				this->_setBlendtoAdditive();
				break;
			case BlendingMode::ALPHA_BLEND:
				this->_setBlendtoAlphaBlend();
				break;
			case BlendingMode::MASK:
				this->_setBlendtoMask();
				break;
			case BlendingMode::MULTIPLY:
				this->_setBlendtoMultiply();
				break;
		}
		return *this;
	}
	GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_viewmask(uint32_t bitmask) {
		_renderInfo.viewMask = bitmask;
		return *this;
	}
}



