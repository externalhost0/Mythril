//
// Created by Hayden Rivas on 10/25/25.
//
#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "../SampleUtils.h"
#include "mythril/RenderGraphBuilder.h"

#include "glm/glm.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

#include <SDL3/SDL_vulkan.h>

#include "Marble.shadergen.h"
#include "../SDL3Usage.h"

struct Camera {
	glm::vec3 position;
	float aspectRatio;
	float fov;
	float nearPlane;
	float farPlane;
};

static shaders::Vertex MakeVertex(const glm::vec3 position, const glm::vec3 normal, const glm::vec2 uv) {
	return {.position = position, .uv_x = uv.x, .normal = normal, .uv_y = uv.y};
}

const std::vector<shaders::Vertex> cubeVertices = {
		// front face
		MakeVertex({-1.f, -1.f, -1.f}, {0.f, 0.f, -1.f}, {0.0f, 0.0f}), // A 0
		MakeVertex({ 1.f, -1.f, -1.f}, {0.f, 0.f, -1.f}, {1.0f, 0.0f}), // B 1
		MakeVertex({ 1.f,  1.f, -1.f}, {0.f, 0.f, -1.f}, {1.0f, 1.0f}), // C 2
		MakeVertex({-1.f,  1.f, -1.f}, {0.f, 0.f, -1.f}, {0.0f, 1.0f}), // D 3

		// back face
		MakeVertex({-1.f, -1.f, 1.f}, {0.f, 0.f, 1.f}, {0.0f, 0.0f}), // E 4
		MakeVertex({ 1.f, -1.f, 1.f}, {0.f, 0.f, 1.f}, {1.0f, 0.0f}), // F 5
		MakeVertex({ 1.f,  1.f, 1.f}, {0.f, 0.f, 1.f}, {1.0f, 1.0f}), // G 6
		MakeVertex({-1.f,  1.f, 1.f}, {0.f, 0.f, 1.f}, {0.0f, 1.0f}), // H 7

		// left face
		MakeVertex({-1.f,  1.f, -1.f}, {-1.f, 0.f, 0.f}, {0.0f, 1.0f}), // D 8
		MakeVertex({-1.f, -1.f, -1.f}, {-1.f, 0.f, 0.f}, {0.0f, 0.0f}), // A 9
		MakeVertex({-1.f, -1.f,  1.f}, {-1.f, 0.f, 0.f}, {1.0f, 0.0f}), // E 10
		MakeVertex({-1.f,  1.f,  1.f}, {-1.f, 0.f, 0.f}, {1.0f, 1.0f}), // H 11

		// right face
		MakeVertex({1.f, -1.f, -1.f}, {1.f, 0.f, 0.f}, {0.0f, 0.0f}), // B 12
		MakeVertex({1.f,  1.f, -1.f}, {1.f, 0.f, 0.f}, {0.0f, 1.0f}), // C 13
		MakeVertex({1.f,  1.f,  1.f}, {1.f, 0.f, 0.f}, {1.0f, 1.0f}), // G 14
		MakeVertex({1.f, -1.f,  1.f}, {1.f, 0.f, 0.f}, {1.0f, 0.0f}), // F 15

		// bottom face
		MakeVertex({-1.f, -1.f, -1.f}, {0.f, -1.f, 0.f}, {0.0f, 1.0f}), // A 16
		MakeVertex({ 1.f, -1.f, -1.f}, {0.f, -1.f, 0.f}, {1.0f, 1.0f}), // B 17
		MakeVertex({ 1.f, -1.f,  1.f}, {0.f, -1.f, 0.f}, {1.0f, 0.0f}), // F 18
		MakeVertex({-1.f, -1.f,  1.f}, {0.f, -1.f, 0.f}, {0.0f, 0.0f}), // E 19

		// top face
		MakeVertex({ 1.f, 1.f, -1.f}, {0.f, 1.f, 0.f}, {1.0f, 1.0f}), // C 20
		MakeVertex({-1.f, 1.f, -1.f}, {0.f, 1.f, 0.f}, {0.0f, 1.0f}), // D 21
		MakeVertex({-1.f, 1.f,  1.f}, {0.f, 1.f, 0.f}, {0.0f, 0.0f}), // H 22
		MakeVertex({ 1.f, 1.f,  1.f}, {0.f, 1.f, 0.f}, {1.0f, 0.0f}), // G 23
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
static glm::mat4 calculateProjectionMatrix(const Camera& camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}


namespace mythril {
	static const char* GetScalarKindString(ScalarKind kind) {
		switch (kind) {
			case ScalarKind::Int:
				return "int";
			case ScalarKind::UInt:
				return "uint";
			case ScalarKind::Float:
			case ScalarKind::Double:
				return "float";
			case ScalarKind::Bool:
				return "bool";
			default:
				return "?";
		}
	}

	static const char* GetAccessString(PushField::Access access) {
		switch (access) {
			case PushField::Access::None:
				return "unused";
			case PushField::Access::Read:
				return "read";
			case PushField::Access::Write:
				return "write";
			case PushField::Access::ReadWrite:
				return "read write";
			default:
				return "unknown";
		}
	}

	static std::string PushTypeString(const PushBlockLayout& block, const PushField& field) {
		const std::string scalar = std::format("{}{}", GetScalarKindString(field.scalar), field.bits);
		switch (field.kind) {
			case PushField::Kind::Scalar: return scalar;
			case PushField::Kind::Vector: return std::format("{}x{}", scalar, field.columns);
			case PushField::Kind::Matrix: return std::format("{} {}x{} {}", scalar, field.rows, field.columns, field.rowMajor ? "row-major" : "column-major");
			case PushField::Kind::Array: return std::format("{} [{}] stride {}", PushTypeString(block, field.fields.front()), field.count, field.stride);
			case PushField::Kind::Struct: return field.typeName;
			case PushField::Kind::Pointer: return std::format("{}* stride {}", PushTypeString(block, block.pointees[field.pointee]), field.stride);
			case PushField::Kind::SampledTexture: return "sampled texture";
			case PushField::Kind::StorageTexture: return "storage texture";
			case PushField::Kind::Sampler: return "sampler";
			case PushField::Kind::AccelerationStructure: return "acceleration structure";
			default: return "?";
		}
	}

	static std::string VkShaderStageToString(VkShaderStageFlags stageFlags) {
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

// a pointer opens to what it points to, ImGui only draws a node once it is open so a pointer to its own struct is fine
void DrawPushField(const mythril::PushBlockLayout& block, const mythril::PushField& field) {
	using Kind = mythril::PushField::Kind;
	ImGui::PushID(&field);
	const mythril::PushField* pointee = field.kind == Kind::Pointer ? &block.pointees[field.pointee] : nullptr;
	const bool hasChildren = (field.kind == Kind::Struct || field.kind == Kind::Array) || (pointee && (pointee->kind == Kind::Struct || pointee->kind == Kind::Array));
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | (hasChildren ? 0 : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
	const bool open = ImGui::TreeNodeEx(field.name.empty() ? "[i]" : field.name.c_str(), flags);
	ImGui::SameLine();
	ImGui::TextDisabled("@%u  %u B  %s", field.offset, field.size, mythril::PushTypeString(block, field).c_str());
	if (field.kind != Kind::Scalar && field.kind != Kind::Vector && field.kind != Kind::Matrix && field.kind != Kind::Array && field.kind != Kind::Struct) {
		ImGui::SameLine();
		ImGui::TextDisabled("| %s", mythril::GetAccessString(field.access));
	}
	if (hasChildren && open) {
		for (const mythril::PushField& child: pointee ? pointee->fields : field.fields)
			DrawPushField(block, child);
		ImGui::TreePop();
	}
	ImGui::PopID();
}
void DrawShaderInfo(const mythril::detail::AllocatedShader& shader) {
	char title_buf[128];
	const char* windowName = "Shader Inspector";
	snprintf(title_buf, sizeof(title_buf), "%s - %s", windowName, shader.getDebugName().data());
	if (ImGui::Begin(title_buf)) {
		ImGui::PushID(&shader);

		const std::span<const mythril::PushBlockLayout> blocks = shader.viewPushBlocks();
		if (ImGui::CollapsingHeader("Push Constants", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Push blocks: %zu", blocks.size());
			for (const VkPushConstantRange& range: shader.getPipelineLayoutSignature().pushes)
				ImGui::Text("Range: offset %u, %u bytes, %s", range.offset, range.size, mythril::VkShaderStageToString(range.stageFlags).c_str());
			ImGui::Separator();
			for (const mythril::PushBlockLayout& block: blocks)
				DrawPushField(block, block.root);
		}
		ImGui::PopID();
	}
	ImGui::End();
}

int main() {
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	SDL_Window* sdlWindow = BuildSDLWindow(false);
	auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
	{
		Uint32 sdlExtensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name"
		})
		.set_window_surface({
			.instanceExtensions = std::span(sdlExtensions, sdlExtensionCount),
			.createFunc = [sdlWindow](VkInstance instance) {
				VkSurfaceKHR surface;
				SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
				return surface;
			},
			.destroyFunc = [](VkInstance instance, VkSurfaceKHR surface) {
				SDL_Vulkan_DestroySurface(instance, surface, nullptr);
			}
		})
		.with_default_swapchain({
			.width = initialWindowSize.width,
			.height = initialWindowSize.height
		})
		.with_ImGui({
			.windowInitFunction = [sdlWindow] { ImGui_ImplSDL3_InitForVulkan(sdlWindow); },
			.windowDestroyFunction = [] { ImGui_ImplSDL3_Shutdown(); }
		})
		.build();

		mythril::Shader standardShader = ctx->createShader({
			.path = kDataDir / shaders::Marble::kSpirvFile,
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
			.size = sizeof(shaders::Vertex) * cubeVertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = cubeVertices,
			.debugName = "Cube Vertex Buffer"
		});
		mythril::Buffer cubeIndexBuffer = ctx->createBuffer({
			.size = sizeof(uint32_t) * cubeIndices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
			.storage = mythril::StorageType::Device,
			.initialData = cubeIndices,
			.debugName = "Cube Index Buffer"
		});
		mythril::Buffer perFrameDataBuffer = ctx->createBuffer({
			.size = sizeof(shaders::GlobalData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "PerFrameData Storage Buffer"
		});
		mythril::Buffer perMaterialDataBuffer = ctx->createBuffer({
			.size = sizeof(shaders::MaterialData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "PerMaterialData Storage Buffer"
		});
		constexpr unsigned int kNumObjects = 50;
		mythril::Buffer objectDataBuf = ctx->createBuffer({
			.size = sizeof(shaders::ObjectData) * kNumObjects,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Object Data SSBO"
		});

		auto startTime = std::chrono::high_resolution_clock::now();
		shaders::ObjectData objects[kNumObjects];
		shaders::MaterialData matData{};
		shaders::GlobalData frameData { .objects = ctx->gpuAddress(objectDataBuf) };

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Color Texture"});
		const mythril::GraphTexture rgDepthTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .debugName = "Depth Texture"});
		const mythril::GraphBuffer rgCubeVertices = graph.importBuffer(cubeVertexBuffer);
		const mythril::GraphBuffer rgCubeIndices = graph.importBuffer(cubeIndexBuffer);
		const mythril::GraphBuffer rgObjectData = graph.importBuffer(objectDataBuf);
		const mythril::GraphBuffer rgPerFrameData = graph.importBuffer(perFrameDataBuffer);
		const mythril::GraphBuffer rgPerMaterialData = graph.importBuffer(perMaterialDataBuffer);

		struct GeometryPass {
			mythril::GraphicsPipelineHandle pipeline;
			mythril::BufferHandle indexBuffer;
			uint32_t indexCount = 0;
			shaders::GeometryPushConstant push{};
		};
		const auto buildGraph = [&] {
			mythril::GraphBuffer objectData = rgObjectData;
			mythril::GraphBuffer perFrameData = rgPerFrameData;
			mythril::GraphBuffer perMaterialData = rgPerMaterialData;
			graph.addTransferPass("per_frame_uploads", [&](mythril::TransferBuilder& b) {
				objectData = b.update(objectData, objects);
				perFrameData = b.update(perFrameData, frameData);
				perMaterialData = b.update(perMaterialData, matData);
			});

			mythril::GraphTexture color = rgColorTarget;
			graph.addGraphicsPass<GeometryPass>("geometry", [&](mythril::GraphicsPassBuilder& b, GeometryPass& d) {
				color = b.attachment({
					.texDesc = color,
					.clearValue = mythril::ClearValue::color(0.2f, 0.2f, 0.2f, 1.f),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.attachment({
					.texDesc = rgDepthTarget,
					.clearValue = mythril::ClearValue::depth(1.f, 0),
					.loadOp = mythril::LoadOp::CLEAR,
				});
				b.dependency(rgCubeVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgCubeIndices, mythril::BufferAccess::IndexRead);
				b.dependency(objectData, mythril::BufferAccess::ShaderRead);
				b.dependency(perFrameData, mythril::BufferAccess::ShaderRead);
				b.dependency(perMaterialData, mythril::BufferAccess::ShaderRead);

				const auto currentTime = std::chrono::high_resolution_clock::now();
				const float time = std::chrono::duration<float>(currentTime - startTime).count();
				// rotating cube!
				glm::mat4 modelmatrix = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
				modelmatrix = glm::rotate(modelmatrix, time * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
				d.pipeline = mainPipeline.handle();
				d.indexBuffer = cubeIndexBuffer.handle();
				d.indexCount = static_cast<uint32_t>(cubeIndices.size());
				d.push = {
					.model = modelmatrix,
					.vertexBufferAddress = ctx->gpuAddress(cubeVertexBuffer),
					.perFrame = ctx->gpuAddress(perFrameDataBuffer),
					.perMaterial = ctx->gpuAddress(perMaterialDataBuffer),
				};
			}, [](const GeometryPass& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
				cmd.cmdPushConstants(d.push);
				cmd.cmdBindIndexBuffer(d.indexBuffer);
				cmd.cmdDrawIndexed(d.indexCount, kNumObjects);
			});

			graph.addGraphicsPass("gui", [&](mythril::GraphicsPassBuilder& b, mythril::NoPassData&) {
				color = b.attachment({
					.texDesc = color,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
			}, [](const mythril::NoPassData&, mythril::GraphicsCmd& cmd) {
				cmd.cmdDrawImGui();
			});

			graph.addTransferPass("presentation", [&](mythril::TransferBuilder& b) {
				b.blit(color, graph.backBuffer());
			});
		};

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
			auto currentWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({
					.width = currentWindowSize.width,
					.height = currentWindowSize.height
				});
			}

			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();
			DrawShaderInfo(ctx->inspectShader(standardShader.handle()));

			ImGui::Begin("Cube Data");
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
			for (shaders::ObjectData& object : objects) {
				glm::vec3 pos = {
					(std::rand() / static_cast<float>(RAND_MAX) - 0.5f) * 100.0f, // range [-50, 50]
					(std::rand() / static_cast<float>(RAND_MAX) - 0.5f) * 100.0f,
					(std::rand() / static_cast<float>(RAND_MAX) - 0.5f) * 100.0f
				};
				glm::vec3 color = {
					std::rand() / static_cast<float>(RAND_MAX), // range [0, 1]
					std::rand() / static_cast<float>(RAND_MAX),
					std::rand() / static_cast<float>(RAND_MAX)
				};
				object = { pos, color };
			}
			objects[0].position = glm::vec3(0.f);


			matData = shaders::MaterialData {
				.tint = { col[0], col[1], col[2] },
				.warpIterations = warp_iterations,
				.glowAmount = glow,
				.distortAmount = distort_amount
			};

			const Camera camera = {
				.position = {0.f, 0.f, 5.f},
				.aspectRatio = static_cast<float>(currentWindowSize.width) / static_cast<float>(currentWindowSize.height),
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 100.f
			};
			const shaders::CameraData cameraData = {
				.proj = calculateProjectionMatrix(camera),
				.view = calculateViewMatrix(camera),
				.position = camera.position
			};
			auto currentTime = std::chrono::high_resolution_clock::now();
			float time = std::chrono::duration<float>(currentTime - startTime).count();
			frameData = shaders::GlobalData{
				.objects = ctx->gpuAddress(objectDataBuf),
				.camera = cameraData,
				.renderResolution = { currentWindowSize.width, currentWindowSize.height },
				.time = time
			};

			graph.reset();
			buildGraph();
			graph.compile(*ctx);
			graph.execute(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}