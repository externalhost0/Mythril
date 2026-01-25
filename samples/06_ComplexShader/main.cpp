//
// Created by Hayden Rivas on 10/25/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/RenderGraphBuilder.h"

#include "glm/glm.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

// for shader structs
#include <fmt/color.h>

#include "GPUStructs.h"
#include "../../lib/vkstring.h"

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

static glm::mat4 calculateViewMatrix(const Camera &camera) {
	return glm::lookAt(camera.position, camera.position + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
}
static glm::mat4 calculateProjectionMatrix(Camera camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}


namespace mythril {
	const char* GetFieldKindString(FieldKind kind) {
		switch (kind) {
			case FieldKind::OpaqueHandle:
				return "OpaqueHandle";
			case FieldKind::Pointer:
				return "Pointer";
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
			case FieldKind::Unknown:
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

void DrawFieldInfoRecursive(const std::vector<mythril::FieldInfo>& fields) {
	for (const auto& field : fields) {
		ImGui::PushID(&field);

		bool isStruct = (field.kind == mythril::FieldKind::Struct);
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
								   (isStruct ? 0 : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));

		std::string label = fmt::format("{} ({})", field.varName, field.typeName);
		bool open = ImGui::TreeNodeEx(label.c_str(), flags);

		if (!isStruct) {
			ImGui::SameLine();
			ImGui::TextDisabled("size=%u | offset=%u | kind=%s",
								field.size, field.offset,
								GetFieldKindString(field.kind));

			switch (field.kind) {
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
									field.size, field.offset, GetFieldKindString(field.kind));

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
void DrawShaderInfo(const mythril::AllocatedShader& shader) {
	char title_buf[128];
	const char* windowName = "Shader Inspector";
	snprintf(title_buf, sizeof(title_buf), "%s - %s", windowName, shader.getDebugName().data());
	if (ImGui::Begin(title_buf)) {
		ImGui::PushID(&shader);

		// binding parameters
		const auto& descriptor_set_infos = shader.viewDescriptorSets();
		if (ImGui::CollapsingHeader("Bound Sets", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Total Sets: %zu", descriptor_set_infos.size());
			ImGui::Separator();

			for (const mythril::AllocatedShader::DescriptorSetInfo& setInfo : descriptor_set_infos) {
				char set_buf[64];
				snprintf(set_buf, sizeof(set_buf), "Set %u", setInfo.setIndex);
				if (ImGui::TreeNode(set_buf)) {
					for (const mythril::AllocatedShader::DescriptorBindingInfo& bindingInfo : setInfo.bindingInfos) {
						ImGui::Text("Variable Name: %s", bindingInfo.varName.c_str());
						ImGui::Text("Type Name: %s", bindingInfo.typeName.c_str());
						ImGui::Text("Set Index: %u", bindingInfo.setIndex);
						ImGui::Text("Binding Index: %u", bindingInfo.bindingIndex);
						ImGui::Text("Descriptor Count: %u", bindingInfo.descriptorCount);
						ImGui::Text("Descriptor Type: %s", mythril::VkDescriptorTypeToString(bindingInfo.descriptorType));
						ImGui::Text("Used Stages: %s", mythril::VkShaderStageToString(bindingInfo.usedStages).c_str());

						// fields
						if (!bindingInfo.fields.empty()) {
							ImGui::Separator();
							DrawFieldInfoRecursive(bindingInfo.fields);
						}
						ImGui::TreePop();

					}
				}
			}
		}

		// push constants
		const auto& pushConstants = shader.viewPushConstants();
		if (ImGui::CollapsingHeader("Push Constants", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Total Push Constants: %zu", pushConstants.size());
			ImGui::Separator();

			for (size_t i = 0; i < pushConstants.size(); i++) {
				const mythril::AllocatedShader::PushConstantInfo& push = pushConstants[i];
				char push_buf[128];
				snprintf(push_buf, sizeof(push_buf), "Push Constant %zu : \"%s\"", i, push.varName.c_str());
				if (ImGui::TreeNode(push_buf)) {
					ImGui::Text("Variable Name: %s", push.varName.c_str());
					ImGui::Text("Type Name: %s", push.typeName.c_str());
					ImGui::Text("Offset: %u", push.offset);
					ImGui::Text("Size: %u bytes", push.size);
					ImGui::Text("Used Stages: %s", mythril::VkShaderStageToString(push.usedStages).c_str());

					// fields
					if (!push.fields.empty()) {
						ImGui::Separator();
						DrawFieldInfoRecursive(push.fields);
					}
					ImGui::TreePop();
				}
			}
		}
		ImGui::PopID();
	}
	ImGui::End();
}

int main() {
	std::vector slang_searchpaths = {
		"../../include/",
		"../include/"
	};
	auto ctx = mythril::CTXBuilder{}
	.set_vulkan_cfg({
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
	.set_slang_cfg({
		.searchpaths = slang_searchpaths
	})
	.with_default_swapchain()
	.with_ImGui()
	.build();

	const VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	const mythril::Dimensions dims = {extent2D.width, extent2D.height, 1};
	mythril::Texture colorTarget = ctx->createTexture({
		.dimension = dims,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.debugName = "Color Texture"
	});
	mythril::Texture finalColorTarget = ctx->createTexture({
		.dimension = dims,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Device,
		.debugName = "Final Color Texture"
	});
	mythril::Texture depthTarget = ctx->createTexture({
		.dimension = dims,
		.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.debugName = "Depth Texture"
	});

	mythril::Shader standardShader = ctx->createShader({
		.filePath = "Marble.slang",
		.debugName = "Example Shader"
	});

	mythril::GraphicsPipeline mainPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.topology = mythril::TopologyMode::TRIANGLE,
		.polygon = mythril::PolygonMode::FILL,
		.blend = mythril::BlendingMode::OFF,
		.cull = mythril::CullMode::BACK,
		.multisample = mythril::SampleCount::X1,
		.debugName = "Geometry Pipeline"
	});

	mythril::Buffer cubeVertexBuffer = ctx->createBuffer({
		.size = sizeof(GPU::Vertex) * cubeVertices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.initialData = cubeVertices.data(),
		.debugName = "Cube Vertex Buffer"
	});
	mythril::Buffer cubeIndexBuffer = ctx->createBuffer({
		.size = sizeof(uint32_t) * cubeIndices.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
		.storage = mythril::StorageType::Device,
		.initialData = cubeIndices.data(),
		.debugName = "Cube Index Buffer"
	});
	mythril::Buffer perFrameDataBuffer = ctx->createBuffer({
		.size = sizeof(GPU::GlobalData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "PerFrameData Uniform Buffer"
	});
	mythril::Buffer perMaterialDataBuffer = ctx->createBuffer({
		.size = sizeof(GPU::MaterialData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "PerMaterialData Uniform Buffer"
	});
	constexpr unsigned int kNumObjects = 50;
	mythril::Buffer objectDataBuf = ctx->createBuffer({
		.size = sizeof(GPU::ObjectData) * kNumObjects,
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.debugName = "Object Data SSBO"
	});

	auto startTime = std::chrono::high_resolution_clock::now();
	mythril::RenderGraph graph;
	graph.addGraphicsPass("geometry")
	.attachment({
		.texDesc = colorTarget,
		.clearValue = {0.2f, 0.2f, 0.2f, 1.f},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.attachment({
		.texDesc = depthTarget,
		.clearValue = {1.f, 0},
		.loadOp = mythril::LoadOperation::CLEAR
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(mainPipeline);

		const auto currentTime = std::chrono::high_resolution_clock::now();
		const float time = std::chrono::duration<float>(currentTime - startTime).count();
		// rotating cube!
		glm::mat4 modelmatrix = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
		modelmatrix = glm::rotate(modelmatrix, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
		GPU::GeometryPushConstant push {
			.model =  modelmatrix,
			.vertexBufferAddress = cubeVertexBuffer.gpuAddress()
		};
		cmd.cmdPushConstants(push);
		cmd.cmdBindIndexBuffer(cubeIndexBuffer);
		cmd.cmdDrawIndexed(cubeIndices.size());
		cmd.cmdEndRendering();
	});

	graph.addGraphicsPass("gui")
	.attachment({
		.texDesc = {colorTarget},
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdDrawImGui();
		cmd.cmdEndRendering();
	});

	graph.addIntermediate("presentation")
	.blit(colorTarget, ctx->getBackBufferTexture())
	.finish();

	graph.compile(*ctx);


	// now we should update descriptor sets for the first & only time
	// we can update multiple descriptor sets at the same time & in the same call
	mythril::DescriptorSetWriter writer = ctx->openDescriptorUpdate(mainPipeline);
	writer.updateBinding(perFrameDataBuffer, "perFrame"); // set 0, binding 0
	writer.updateBinding(perMaterialDataBuffer, "perMaterial"); // set 1, binding 0
	ctx->submitDescriptorUpdate(writer);

	std::srand(std::time(nullptr));
	bool quit = false;
	while(!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT) quit = true;
			if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.key == SDLK_Q) {
					quit = true;
				}
			}
		}

		// mandatory for resizeability
		// or else your presentation will break
		if (ctx->isSwapchainDirty()) {
			ctx->recreateSwapchain();

			const mythril::Window& window = ctx->getWindow();
			// get framebuffer size for correct resolution, not windowsize which might not scale to your monitor dpi correctly
			VkExtent2D new_extent_2d = window.getFramebufferSize();
			mythril::Dimensions applied_dimensions = { new_extent_2d.width, new_extent_2d.height, 1};
			colorTarget.resize(applied_dimensions);
			depthTarget.resize(applied_dimensions);
			finalColorTarget.resize(applied_dimensions);
			// you must recompile your framegraph in order for it to recieve texture changes
			graph.compile(*ctx);
		}

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		DrawShaderInfo(standardShader.view());

		ImGui::Begin("Cube Uniforms");
		ImGui::Text("Original Shader design by 'nasana'.");
		ImGui::TextLinkOpenURL("Click Here For Their Work", "https://www.shadertoy.com/view/WtdXR8");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		static float col[3] = {1.f, 0.f, 0.f };
		ImGui::ColorPicker3("Tint Color", col);
		static float distort_amount = 0.6f;
		ImGui::SliderFloat("Distort Amount", &distort_amount, 0.f, 1.f);
		static float glow = 0.5f;
		ImGui::SliderFloat("Glow", &glow, 0.f, 1.f);
		static float warp_iterations = 1.0f;
		ImGui::SliderFloat("Warp Iterations", &warp_iterations, 0.f, 1.f);
		ImGui::End();

		std::srand(static_cast<unsigned>(std::time(nullptr)));
		GPU::ObjectData objects[kNumObjects];
		for (GPU::ObjectData& object : objects) {
			glm::vec3 pos = {
					(std::rand() / (float)RAND_MAX - 0.5f) * 100.0f, // range [-50, 50]
					(std::rand() / (float)RAND_MAX - 0.5f) * 100.0f,
					(std::rand() / (float)RAND_MAX - 0.5f) * 100.0f
			};

			glm::vec3 color = {
					std::rand() / (float)RAND_MAX, // range [0, 1]
					std::rand() / (float)RAND_MAX,
					std::rand() / (float)RAND_MAX
			};
			object = { pos, color };
		}


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
		// once again, getFramebufferSize is the actual resolution we render out
		const VkExtent2D renderSize = ctx->getWindow().getFramebufferSize();
		const GPU::GlobalData frameData {
			.objects = objectDataBuf.gpuAddress(),
			.camera = cameraData,
			.renderResolution = { renderSize.width, renderSize.height },
			.time = time
		};

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);

		// buffer updating must be done before rendering
		// todo: make this part of the rendergraph
		cmd.cmdUpdateBuffer(objectDataBuf, objects);
		cmd.cmdUpdateBuffer(perFrameDataBuffer, frameData);
		cmd.cmdUpdateBuffer(perMaterialDataBuffer, matData);

		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}
	return 0;
}