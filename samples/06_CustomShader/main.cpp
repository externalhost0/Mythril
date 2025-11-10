//
// Created by Hayden Rivas on 10/25/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"

#include "glm/glm.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_sdl3.h"

#include "SDL3/SDL.h"

// for shader structs
#include "GPUStructs.h"

struct Camera {
	glm::vec3 position;
	float aspectRatio;
	float fov;
	float nearPlane;
	float farPlane;
};

const std::vector<GPU::Vertex> cubeVertices = {
		// front face
		{{-1.f, -1.f, -1.f}, {0.f, 0.f, -1.f}, {0.0f, 0.0f}}, // A 0
		{{ 1.f, -1.f, -1.f}, {0.f, 0.f, -1.f}, {1.0f, 0.0f}}, // B 1
		{{ 1.f,  1.f, -1.f}, {0.f, 0.f, -1.f}, {1.0f, 1.0f}}, // C 2
		{{-1.f,  1.f, -1.f}, {0.f, 0.f, -1.f}, {0.0f, 1.0f}}, // D 3

		// back face
		{{-1.f, -1.f, 1.f}, {0.f, 0.f, 1.f}, {0.0f, 0.0f}}, // E 4
		{{ 1.f, -1.f, 1.f}, {0.f, 0.f, 1.f}, {1.0f, 0.0f}}, // F 5
		{{ 1.f,  1.f, 1.f}, {0.f, 0.f, 1.f}, {1.0f, 1.0f}}, // G 6
		{{-1.f,  1.f, 1.f}, {0.f, 0.f, 1.f}, {0.0f, 1.0f}}, // H 7

		// left face
		{{-1.f,  1.f, -1.f}, {-1.f, 0.f, 0.f}, {0.0f, 1.0f}}, // D 8
		{{-1.f, -1.f, -1.f}, {-1.f, 0.f, 0.f}, {0.0f, 0.0f}}, // A 9
		{{-1.f, -1.f,  1.f}, {-1.f, 0.f, 0.f}, {1.0f, 0.0f}}, // E 10
		{{-1.f,  1.f,  1.f}, {-1.f, 0.f, 0.f}, {1.0f, 1.0f}}, // H 11

		// right face
		{{1.f, -1.f, -1.f}, {1.f, 0.f, 0.f}, {0.0f, 0.0f}}, // B 12
		{{1.f,  1.f, -1.f}, {1.f, 0.f, 0.f}, {0.0f, 1.0f}}, // C 13
		{{1.f,  1.f,  1.f}, {1.f, 0.f, 0.f}, {1.0f, 1.0f}}, // G 14
		{{1.f, -1.f,  1.f}, {1.f, 0.f, 0.f}, {1.0f, 0.0f}}, // F 15

		// bottom face
		{{-1.f, -1.f, -1.f}, {0.f, -1.f, 0.f}, {0.0f, 1.0f}}, // A 16
		{{ 1.f, -1.f, -1.f}, {0.f, -1.f, 0.f}, {1.0f, 1.0f}}, // B 17
		{{ 1.f, -1.f,  1.f}, {0.f, -1.f, 0.f}, {1.0f, 0.0f}}, // F 18
		{{-1.f, -1.f,  1.f}, {0.f, -1.f, 0.f}, {0.0f, 0.0f}}, // E 19

		// top face
		{{ 1.f, 1.f, -1.f}, {0.f, 1.f, 0.f}, {1.0f, 1.0f}}, // C 20
		{{-1.f, 1.f, -1.f}, {0.f, 1.f, 0.f}, {0.0f, 1.0f}}, // D 21
		{{-1.f, 1.f,  1.f}, {0.f, 1.f, 0.f}, {0.0f, 0.0f}}, // H 22
		{{ 1.f, 1.f,  1.f}, {0.f, 1.f, 0.f}, {1.0f, 0.0f}}, // G 23
};


const std::vector<uint32_t> cubeIndices = {
		// front and back
		0, 3, 2,
		2, 1, 0,
		4, 5, 6,
		6, 7, 4,
		// left and right
		11, 8, 9,
		9, 10, 11,
		12, 13, 14,
		14, 15, 12,
		// bottom and top
		16, 17, 18,
		18, 19, 16,
		20, 21, 22,
		22, 23, 20
};

glm::mat4 calculateViewMatrix(Camera camera) {
	return glm::lookAt(camera.position, camera.position + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
}
glm::mat4 calculateProjectionMatrix(Camera camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}


namespace mythril {
	const char* GetFieldKindString(FieldKind kind) {
		switch (kind) {
			case FieldKind::OpaqueHandle:
				return "OpaqueHandle";
			case FieldKind::Scalar:
				return "Scalar";
			case FieldKind::Vector:
				return "Vector";
			case FieldKind::Matrix:
				return "Matrix";
			case FieldKind::Array:
				return "Array";
			case FieldKind::Struct:
				return "Struct";
			default:
				return "Unknown";
		}
	}

	const char* GetScalarKindString(ScalarKind kind) {
		switch (kind) {
			case ScalarKind::None:
				return "None";
			case ScalarKind::Int:
				return "Int";
			case ScalarKind::UInt:
				return "UInt";
			case ScalarKind::Float:
				return "Float";
			case ScalarKind::Double:
				return "Double";
			case ScalarKind::Bool:
				return "Bool";
			default:
				return "Unknown";
		}
	}

	const char* GetOpaqueKindString(OpaqueKind kind) {
		switch (kind) {
			case OpaqueKind::Sampler:
				return "Sampler";
			case OpaqueKind::Texture1D:
				return "Texture1D";
			case OpaqueKind::Texture2D:
				return "Texture2D";
			case OpaqueKind::Texture3D:
				return "Texture3D";
			case OpaqueKind::TextureCube:
				return "TextureCube";
			case OpaqueKind::RWTexture1D:
				return "RWTexture1D";
			case OpaqueKind::RWTexture2D:
				return "RWTexture2D";
			case OpaqueKind::RWTexture3D:
				return "RWTexture3D";
			default:
				return "Unknown";
		}
	}

	const char* VkDescriptorTypeToString(VkDescriptorType type) {
		switch (type) {
			case VK_DESCRIPTOR_TYPE_SAMPLER:
				return "VK_DESCRIPTOR_TYPE_SAMPLER";
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER";
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE";
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE";
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER";
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				return "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER";
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER";
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER";
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
				return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC";
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC";
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				return "VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT";
			case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
				return "VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK";
			case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
				return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR";
			case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
				return "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV";
			case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
				return "VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM";
			case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
				return "VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM";
			case VK_DESCRIPTOR_TYPE_TENSOR_ARM:
				return "VK_DESCRIPTOR_TYPE_TENSOR_ARM";
			case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
				return "VK_DESCRIPTOR_TYPE_MUTABLE_EXT";
			case VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV:
				return "VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV";
			case VK_DESCRIPTOR_TYPE_MAX_ENUM:
				return "VK_DESCRIPTOR_TYPE_MAX_ENUM";
		}
	}


	std::string VkShaderStageToString(VkShaderStageFlags stageFlags) {
		std::string result;

		if (stageFlags & VK_SHADER_STAGE_VERTEX_BIT)
			result += "VERTEX, ";
		if (stageFlags & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
			result += "TESSELLATION_CONTROL, ";
		if (stageFlags & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
			result += "TESSELLATION_EVALUATION, ";
		if (stageFlags & VK_SHADER_STAGE_GEOMETRY_BIT)
			result += "GEOMETRY, ";
		if (stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT)
			result += "FRAGMENT, ";
		if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
			result += "COMPUTE, ";
		if (stageFlags & VK_SHADER_STAGE_TASK_BIT_EXT)
			result += "TASK, ";
		if (stageFlags & VK_SHADER_STAGE_MESH_BIT_EXT)
			result += "MESH, ";
		if (stageFlags & VK_SHADER_STAGE_RAYGEN_BIT_KHR)
			result += "RAYGEN, ";
		if (stageFlags & VK_SHADER_STAGE_ANY_HIT_BIT_KHR)
			result += "ANY_HIT, ";
		if (stageFlags & VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
			result += "CLOSEST_HIT, ";
		if (stageFlags & VK_SHADER_STAGE_MISS_BIT_KHR)
			result += "MISS, ";
		if (stageFlags & VK_SHADER_STAGE_INTERSECTION_BIT_KHR)
			result += "INTERSECTION, ";
		if (stageFlags & VK_SHADER_STAGE_CALLABLE_BIT_KHR)
			result += "CALLABLE, ";
		if (stageFlags & VK_SHADER_STAGE_SUBPASS_SHADING_BIT_HUAWEI)
			result += "SUBPASS_SHADING_HUAWEI, ";
		if (stageFlags & VK_SHADER_STAGE_CLUSTER_CULLING_BIT_HUAWEI)
			result += "CLUSTER_CULLING_HUAWEI, ";

		if (stageFlags == VK_SHADER_STAGE_ALL_GRAPHICS)
			result += "ALL_GRAPHICS, ";
		else if (stageFlags == VK_SHADER_STAGE_ALL)
			result += "ALL, ";

		if (result.empty())
			result = "UNKNOWN";
		else
			// remove last ", "
			result.erase(result.size() - 2);

		return result;
	}
}

void DrawFieldInfoRecursive(const std::vector<mythril::FieldInfo>& fields);

void DrawShaderInfo(const mythril::Shader &shader) {
	char buf[128];
	const char* windowName = "Shader Inspector";
	snprintf(buf, sizeof(buf), "Shader Inspector - %s", shader._debugName);
	if (ImGui::Begin(buf)) {
		ImGui::PushID(shader._debugName);

		// shader module info
		if (ImGui::CollapsingHeader("Pipeline Information", ImGuiTreeNodeFlags_DefaultOpen)) {
//			ImGui::Text("Shader Module: 0x%llx", (uint64_t) shader.vkShaderModule);
//			ImGui::Text("Pipeline Layout: 0x%llx", (uint64_t) shader.vkPipelineLayout);
			ImGui::Separator();
		}

		// binding parameters
		const auto& params = shader.viewParameters();
		if (ImGui::CollapsingHeader("Bound Sets", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Total Sets: %zu", params.size());
			ImGui::Separator();

			for (size_t i = 0; i < params.size(); i++) {
				const auto& paramater = params[i];

				std::string label = "Set " + std::to_string(i) + ": " + paramater.varName;
				if (ImGui::TreeNode(label.c_str())) {
					ImGui::Text("Variable Name: %s", paramater.varName.c_str());
					ImGui::Text("Type Name: %s", paramater.typeName.c_str());
					ImGui::Text("Complete Slang Name: %s", paramater.completeSlangName.c_str());
					ImGui::Text("Set Index: %u", paramater.setIndex);
					ImGui::Text("Binding Index: %u", paramater.bindingIndex);
					ImGui::Text("Descriptor Count: %u", paramater.descriptorCount);
					ImGui::Text("Descriptor Type: %s", mythril::VkDescriptorTypeToString(paramater.descriptorType));
					ImGui::Text("Used Stages: %s", mythril::VkShaderStageToString(paramater.usedStages).c_str());

					// fields
					if (!paramater.fields.empty()) {
						ImGui::Separator();
						DrawFieldInfoRecursive(paramater.fields);
					}
					ImGui::TreePop();
				}
			}
		}

		// push constants
		auto pushConstants = shader.viewPushConstants();
		if (ImGui::CollapsingHeader("Push Constants", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Total Push Constants: %zu", pushConstants.size());
			ImGui::Separator();

			for (size_t i = 0; i < pushConstants.size(); i++) {
				const mythril::Shader::PushConstantInfo& pc = pushConstants[i];

				std::string label = "Push Constant " + std::to_string(i) + ": " + pc.varName;
				if (ImGui::TreeNode(label.c_str())) {
					ImGui::Text("Variable Name: %s", pc.varName.c_str());
					ImGui::Text("Type Name: %s", pc.typeName.c_str());
					ImGui::Text("Complete Slang Name: %s", pc.completeSlangName.c_str());
					ImGui::Text("Offset: %u", pc.offset);
					ImGui::Text("Size: %u bytes", pc.size);
					ImGui::Text("Used Stages: %s", mythril::VkShaderStageToString(pc.usedStages).c_str());

					// fields
					if (!pc.fields.empty()) {
						ImGui::Separator();
						DrawFieldInfoRecursive(pc.fields);
					}
					ImGui::TreePop();
				}
			}
		}
		ImGui::PopID();
	}
	ImGui::End();
}
void DrawFieldInfoRecursive(const std::vector<mythril::FieldInfo>& fields) {
	for (const auto& field : fields) {
		ImGui::PushID(&field);

		bool isStruct = (field.fieldKind == mythril::FieldKind::Struct);
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
								   (isStruct ? 0 : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));

		std::string label = fmt::format("{} ({})", field.varName, field.typeName);
		bool open = ImGui::TreeNodeEx(label.c_str(), flags);

		if (!isStruct) {
			ImGui::SameLine();
			ImGui::TextDisabled("size=%u | offset=%u | kind=%s",
								field.size, field.offset,
								GetFieldKindString(field.fieldKind));

			switch (field.fieldKind) {
				case mythril::FieldKind::Scalar:
					ImGui::SameLine();
					ImGui::TextDisabled("| scalar=%s", GetScalarKindString(field.scalarKind));
					break;
				case mythril::FieldKind::OpaqueHandle:
					ImGui::SameLine();
					ImGui::TextDisabled("| opaque=%s", GetOpaqueKindString(field.opaqueKind));
					break;
				case mythril::FieldKind::Vector:
					ImGui::SameLine();
					ImGui::TextDisabled("| components=%u", field.componentCount);
					break;
				case mythril::FieldKind::Matrix:
					ImGui::SameLine();
					ImGui::TextDisabled("| %ux%u", field.rowCount, field.columnCount);
					break;
				default: break;
			}
		}
		else {
			if (open) {
				ImGui::TextDisabled("size=%u bytes | offset=%u | kind=%s",
									field.size, field.offset, GetFieldKindString(field.fieldKind));

				if (!field.fields.empty()) {
					ImGui::SeparatorText("Members");
					DrawFieldInfoRecursive(field.fields);
				}
			}
		}
		if (isStruct && open)
			ImGui::TreePop();

		ImGui::PopID();
	}
}

int main() {
	auto ctx = mythril::CTXBuilder{}
	.set_info_spec({
		.app_name = "Cool App Name",
		.engine_name = "Cool Engine Name"
	})
	.set_window_spec({
		.title = "Cool Window Name",
		.mode = mythril::WindowMode::Windowed,
		.width = 1280,
		.height = 720,
		.resizeable = true
	})
	.set_shader_search_paths({
		"../../include/"
	})
	.with_ImGui()
	.build();

	VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	mythril::InternalTextureHandle colorTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Color Texture"
	});
	mythril::InternalTextureHandle emissiveTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Emissive Texture"
	});
	mythril::InternalTextureHandle horizBlurredTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Horizontally Blurred Color Target"
	});
	mythril::InternalTextureHandle postColorTarget = ctx->createTexture({
		.dimension = extent2D,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Final Color Texture"
	});
	mythril::InternalTextureHandle depthTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.debugName = "Depth Texture"
	});

	mythril::InternalShaderHandle standardShader = ctx->createShader({
		.filePath = "Marble.slang",
		.debugName = "Example Shader"
	});
	mythril::DescriptorSet set1 = ctx.requestSet(standardShader, 1);
	mythril::InternalGraphicsPipelineHandle mainPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.topology = mythril::TopologyMode::TRIANGLE,
		.polygon = mythril::PolygonMode::FILL,
		.blend = mythril::BlendingMode::OFF,
		.cull = mythril::CullMode::BACK,
		.multisample = mythril::SampleCount::X1,
		.debugName = "Main Pipeline"
	});
	mythril::InternalShaderHandle postProcessingShader = ctx->createShader({
		.filePath = "GaussianBlur.slang",
		.debugName = "Fullscreen Shader"
	});
	mythril::InternalGraphicsPipelineHandle postPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {postProcessingShader},
		.fragmentShader = {postProcessingShader},
		.debugName = "Post Processing Pipeline"
	});

	mythril::InternalBufferHandle cubeVertexBuffer = ctx->createBuffer({
		.size = sizeof(GPU::Vertex) * cubeVertices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.initialData = cubeVertices.data(),
		.debugName = "Cube Vertex Buffer"
	});
	mythril::InternalBufferHandle cubeIndexBuffer = ctx->createBuffer({
		.size = sizeof(uint32_t) * cubeIndices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
		.storage = mythril::StorageType::Device,
		.initialData = cubeIndices.data(),
		.debugName = "Cube Index Buffer"
	});

	mythril::InternalBufferHandle perFrameDataBuffer = ctx->createBuffer({
		.size = sizeof(GPU::GlobalData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "PerFrameData Uniform Buffer"
	});
	mythril::InternalBufferHandle perMaterialDataBuffer = ctx->createBuffer({
		.size = sizeof(GPU::MaterialData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "PerMaterialData Uniform Buffer"
	});


	auto startTime = std::chrono::high_resolution_clock::now();
	mythril::RenderGraph graph;
	graph.addPass("geometry", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.clearValue = {0.2f, 0.2f, 0.2f, 1.f},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.write({
		.texture = depthTarget,
		.clearValue = {0, 1},
		.loadOp = mythril::LoadOperation::CLEAR
	})
	.write({
		.texture = emissiveTarget,
		.clearValue = {0, 0, 0, 1},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(mainPipeline);

		ctx->updateDescriptorSet(perFrameDataBuffer, "perFrame");
		ctx->updateDescriptorSet(perMaterialDataBuffer, "perMaterial");

		auto currentTime = std::chrono::high_resolution_clock::now();
		float time = std::chrono::duration<float>(currentTime - startTime).count();
		// rotating cube!
		glm::mat4 modelmatrix = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
		modelmatrix = glm::rotate(modelmatrix, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
		// std430
		const VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		const Camera camera = {
				.position = {0.f, 0.f, 5.f},
				.aspectRatio = (float) windowSize.width / (float) windowSize.height,
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 100.f
		};

		GPU::GeometryPushConstant push {
			.model = calculateProjectionMatrix(camera) * calculateViewMatrix(camera) * modelmatrix,
			.vertexBufferAddress = ctx->gpuAddress(cubeVertexBuffer)
		};
		cmd.cmdPushConstants(push);
		cmd.cmdBindIndexBuffer(cubeIndexBuffer);
		cmd.cmdDrawIndexed(cubeIndices.size());
	});

	float scale = 2.4f;
	float intensity = 1.f;

	graph.addPass("horizontal_blur", mythril::PassSource::Type::Graphics)
	.write({
		.texture = horizBlurredTarget,
		.clearValue = {0, 0, 0, 1},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.read({
		.texture = colorTarget
	})
	.read({
		.texture = emissiveTarget
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd){
		cmd.cmdBindRenderPipeline(postPipeline);
		GPU::GaussianPushConstant push {
			.colorTexture = colorTarget.index(),
			.emissiveTexture = emissiveTarget.index(),
			.samplerId = 0,
			.scale = scale,
			.intensity = intensity,
			.blurdirection = 1
		};
		cmd.cmdPushConstants(push);
		cmd.cmdDraw(3);
	});

	graph.addPass("vertical_blur", mythril::PassSource::Type::Graphics)
	.write({
		.texture = postColorTarget,
		.clearValue = {0, 0, 0, 1},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.read({
		.texture = colorTarget
	})
	.read({
		.texture = horizBlurredTarget
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd){
		cmd.cmdBindRenderPipeline(postPipeline);

		GPU::GaussianPushConstant push {
			.colorTexture = colorTarget.index(),
			.emissiveTexture = horizBlurredTarget.index(),
			.samplerId = 0,
			.scale = scale,
			.intensity = intensity,
			.blurdirection = 0
		};
		cmd.cmdPushConstants(push);
		cmd.cmdDraw(3);
	});

	graph.addPass("gui", mythril::PassSource::Type::Graphics)
	.write({
		.texture = postColorTarget,
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdDrawImGui();
	});

	graph.compile(*ctx);


	bool quit = false;
	while(!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT) quit = true;
		}

		// mandatory for resizeability
		if (ctx->isSwapchainDirty()) {
			ctx->cleanSwapchain();

			const mythril::Window& window = ctx->getWindow();
			// get framebuffer size for correct resolution, not windowsize which might not scale to your monitor dpi correctly
			extent2D = window.getFramebufferSize();
			ctx->resizeTexture(colorTarget, extent2D);
			ctx->resizeTexture(depthTarget, extent2D);
			ctx->resizeTexture(postColorTarget, extent2D);
			ctx->resizeTexture(emissiveTarget, extent2D);
			ctx->resizeTexture(horizBlurredTarget, extent2D);
			graph.compile(*ctx);
		}


		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		DrawShaderInfo(ctx->viewShader(standardShader));
		DrawShaderInfo(ctx->viewShader(postProcessingShader));

		ImGui::Begin("Post Processing Uniforms");
		ImGui::SliderFloat("Bloom Scale", &scale, 0.f, 50.f);
		ImGui::SliderFloat("Intensity", &intensity, 0.f, 2.f);
		ImGui::End();

		ImGui::Begin("Cube Uniforms");
		static float col[3] = {1.f, 0.f, 0.f };
		ImGui::ColorPicker3("Tint Color", col);
		static float distort_amount = 0.6f;
		ImGui::SliderFloat("Distort Amount", &distort_amount, 0.f, 1.f);
		static float glow = 0.5f;
		ImGui::SliderFloat("Glow", &glow, 0.f, 1.f);
		static float warp_iterations = 1.0f;
		ImGui::SliderFloat("Warp Iterations", &warp_iterations, 0.f, 1.f);
		ImGui::End();

		const GPU::MaterialData matData = {
				.tint = { col[0], col[1], col[2] },
				.warpIterations = warp_iterations,
				.glowAmount = glow,
				.distortAmount = distort_amount
		};

		const VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		const Camera camera = {
				.position = {0.f, 0.f, 5.f},
				.aspectRatio = (float) windowSize.width / (float) windowSize.height,
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 100.f
		};
		const GPU::CameraData cameraData = {
				.proj = calculateProjectionMatrix(camera),
				.view = calculateViewMatrix(camera),
				.position = camera.position
		};
		auto currentTime = std::chrono::high_resolution_clock::now();
		float time = std::chrono::duration<float>(currentTime - startTime).count();
		// once again, getFramebufferSize is the actual resolution we render out,
		// though it might not be the same size as the surface we are displaying it to
		const VkExtent2D renderSize = ctx->getWindow().getFramebufferSize();
		const GPU::GlobalData frameData {
				.camera = cameraData,
				.resolution = { renderSize.width, renderSize.height },
				.time = time
		};
		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		// buffer updating must be done before rendering
		cmd.cmdUpdateBuffer(perFrameDataBuffer, frameData);
		cmd.cmdUpdateBuffer(perMaterialDataBuffer, matData);

		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}
