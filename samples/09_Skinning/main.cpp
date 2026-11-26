//
// Created by Hayden Rivas on 11/17/26.
//
#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "../SampleUtils.h"
#include "mythril/RenderGraphBuilder.h"

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <imgui.h>
#include <ImGuizmo.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <optional>

#include "GPUStructs.h"
#include "Skinning.shadergen.h"
#include "../SDL3Usage.h"

struct Node {
	std::string name;
	int parent = -1;
	std::vector<size_t> children;
	glm::vec3 translation{0.f};
	glm::quat rotation{1.f, 0.f, 0.f, 0.f};
	glm::vec3 scale{1.f};
	std::optional<glm::mat4> matrix;
};

struct Channel {
	size_t node{};
	fastgltf::AnimationPath path{};
	std::vector<float> times;
	std::vector<glm::vec4> values;
};

struct SkinnedModel {
	std::vector<mythril::GPU::SkinnedVertex> vertices;
	std::vector<uint32_t> indices;

	std::vector<Node> nodes;
	std::vector<size_t> parentFirstOrder;
	size_t meshNode = 0;

	std::vector<size_t> joints;
	std::vector<glm::mat4> inverseBindMatrices;

	std::vector<Channel> channels;
	float duration = 0.f;
};

static SkinnedModel loadSkinnedModel(const std::filesystem::path& filepath) {
	auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
	SAMPLE_ASSERT_MSG(data.error() == fastgltf::Error::None, "Failed to read {}", filepath.string());
	fastgltf::Parser parser;
	auto asset = parser.loadGltfBinary(data.get(), filepath.parent_path(), fastgltf::Options::LoadExternalBuffers);
	SAMPLE_ASSERT_MSG(asset.error() == fastgltf::Error::None, "Failed to parse {}", filepath.string());
	const fastgltf::Asset& gltf = asset.get();
	SAMPLE_ASSERT_MSG(gltf.skins.size() == 1, "Sample expects exactly one skin");

	SkinnedModel model;

	model.nodes.resize(gltf.nodes.size());
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		const fastgltf::Node& src = gltf.nodes[i];
		Node& dst = model.nodes[i];
		dst.name = src.name;
		dst.children.assign(src.children.begin(), src.children.end());
		for (size_t child: src.children)
			model.nodes[child].parent = static_cast<int>(i);
		std::visit(fastgltf::visitor{
					   [&](const fastgltf::TRS& trs) {
						   dst.translation = {trs.translation[0], trs.translation[1], trs.translation[2]};
						   dst.rotation = glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
						   dst.scale = {trs.scale[0], trs.scale[1], trs.scale[2]};
					   },
					   [&](const fastgltf::math::fmat4x4& m) {
						   glm::mat4 mat;
						   std::memcpy(&mat, m.data(), sizeof(mat));
						   dst.matrix = mat;
					   }},
			src.transform);
		if (src.meshIndex.has_value())
			model.meshNode = i;
	}
	const auto visit = [&](auto& self, size_t node) -> void {
		model.parentFirstOrder.push_back(node);
		for (size_t child: model.nodes[node].children)
			self(self, child);
	};
	for (size_t root: gltf.scenes[gltf.defaultScene.value_or(0)].nodeIndices)
		visit(visit, root);

	const fastgltf::Skin& skin = gltf.skins[0];
	model.joints.assign(skin.joints.begin(), skin.joints.end());
	model.inverseBindMatrices.resize(model.joints.size(), glm::mat4(1.f));
	if (skin.inverseBindMatrices.has_value())
		fastgltf::copyFromAccessor<glm::mat4>(gltf, gltf.accessors[skin.inverseBindMatrices.value()], model.inverseBindMatrices.data());

	const fastgltf::Primitive& primitive = gltf.meshes[gltf.nodes[model.meshNode].meshIndex.value()].primitives[0];
	const auto accessorFor = [&](const char* name) -> const fastgltf::Accessor& {
		const auto* attribute = primitive.findAttribute(name);
		SAMPLE_ASSERT_MSG(attribute != primitive.attributes.end(), "Mesh is missing {}", name);
		return gltf.accessors[attribute->accessorIndex];
	};
	model.vertices.resize(accessorFor("POSITION").count);
	fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessorFor("POSITION"), [&](glm::vec3 v, size_t i) { model.vertices[i].position = v; });
	fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessorFor("NORMAL"), [&](glm::vec3 v, size_t i) { model.vertices[i].normal = v; });
	fastgltf::iterateAccessorWithIndex<glm::uvec4>(gltf, accessorFor("JOINTS_0"), [&](glm::uvec4 v, size_t i) { model.vertices[i].joints = v; });
	fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, accessorFor("WEIGHTS_0"), [&](glm::vec4 v, size_t i) {
		const float sum = v.x + v.y + v.z + v.w;
		model.vertices[i].weights = sum > 0.f ? v / sum : glm::vec4(1.f, 0.f, 0.f, 0.f);
	});
	fastgltf::iterateAccessor<uint32_t>(gltf, gltf.accessors[primitive.indicesAccessor.value()], [&](uint32_t idx) { model.indices.push_back(idx); });

	if (!gltf.animations.empty()) {
		const fastgltf::Animation& animation = gltf.animations[0];
		for (const fastgltf::AnimationChannel& srcChannel: animation.channels) {
			if (!srcChannel.nodeIndex.has_value() || srcChannel.path == fastgltf::AnimationPath::Weights)
				continue;
			const fastgltf::AnimationSampler& sampler = animation.samplers[srcChannel.samplerIndex];
			SAMPLE_ASSERT_MSG(sampler.interpolation == fastgltf::AnimationInterpolation::Linear, "Sample only handles linear keyframes");
			Channel& channel = model.channels.emplace_back();
			channel.node = srcChannel.nodeIndex.value();
			channel.path = srcChannel.path;
			fastgltf::iterateAccessor<float>(gltf, gltf.accessors[sampler.inputAccessor], [&](float t) { channel.times.push_back(t); });
			const fastgltf::Accessor& output = gltf.accessors[sampler.outputAccessor];
			if (channel.path == fastgltf::AnimationPath::Rotation)
				fastgltf::iterateAccessor<glm::vec4>(gltf, output, [&](glm::vec4 v) { channel.values.push_back(v); });
			else
				fastgltf::iterateAccessor<glm::vec3>(gltf, output, [&](glm::vec3 v) { channel.values.emplace_back(v, 0.f); });
			model.duration = std::max(model.duration, channel.times.back());
		}
	}
	return model;
}

static glm::vec4 sampleChannel(const Channel& channel, float time) {
	if (time <= channel.times.front()) return channel.values.front();
	if (time >= channel.times.back()) return channel.values.back();
	const size_t next = std::upper_bound(channel.times.begin(), channel.times.end(), time) - channel.times.begin();
	const size_t prev = next - 1;
	const float alpha = (time - channel.times[prev]) / (channel.times[next] - channel.times[prev]);
	const glm::vec4& a = channel.values[prev];
	const glm::vec4& b = channel.values[next];
	if (channel.path != fastgltf::AnimationPath::Rotation)
		return glm::mix(a, b, alpha);
	const glm::quat q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), alpha);
	return {q.x, q.y, q.z, q.w};
}

struct Pose {
	std::vector<glm::mat4> globals;
	std::vector<glm::mat4> jointMatrices;
	std::vector<mythril::GPU::DualQuat> jointDualQuats;
};

static Pose evaluatePose(const SkinnedModel& model, float time, std::span<const glm::quat> rotationOffsets) {
	Pose pose;
	std::vector<Node> locals = model.nodes;
	for (const Channel& channel: model.channels) {
		const glm::vec4 v = sampleChannel(channel, time);
		Node& node = locals[channel.node];
		switch (channel.path) {
			case fastgltf::AnimationPath::Translation: node.translation = glm::vec3(v); break;
			case fastgltf::AnimationPath::Rotation: node.rotation = glm::quat(v.w, v.x, v.y, v.z); break;
			case fastgltf::AnimationPath::Scale: node.scale = glm::vec3(v); break;
			default: break;
		}
	}
	for (size_t slot = 0; slot < model.joints.size(); slot++)
		locals[model.joints[slot]].rotation *= rotationOffsets[slot];

	pose.globals.resize(model.nodes.size());
	for (size_t n: model.parentFirstOrder) {
		const Node& node = locals[n];
		const glm::mat4 local = node.matrix.value_or(
			glm::translate(glm::mat4(1.f), node.translation) * glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale));
		pose.globals[n] = node.parent >= 0 ? pose.globals[node.parent] * local : local;
	}

	const glm::mat4 inverseMesh = glm::inverse(pose.globals[model.meshNode]);
	pose.jointMatrices.resize(model.joints.size());
	pose.jointDualQuats.resize(model.joints.size());
	for (size_t slot = 0; slot < model.joints.size(); slot++) {
		const glm::mat4 m = inverseMesh * pose.globals[model.joints[slot]] * model.inverseBindMatrices[slot];
		pose.jointMatrices[slot] = m;
		const glm::quat real = glm::normalize(glm::quat_cast(glm::mat3(m)));
		const glm::quat dual = 0.5f * glm::quat(0.f, glm::vec3(m[3])) * real;
		pose.jointDualQuats[slot] = {
			.real = {real.x, real.y, real.z, real.w},
			.dual = {dual.x, dual.y, dual.z, dual.w}};
	}
	return pose;
}

static std::optional<ImVec2> projectToScreen(const glm::mat4& viewProj, const glm::vec3& world, ImVec2 display) {
	const glm::vec4 clip = viewProj * glm::vec4(world, 1.f);
	if (clip.w <= 0.f) return std::nullopt;
	const glm::vec2 ndc = glm::vec2(clip) / clip.w;
	return ImVec2{(ndc.x * 0.5f + 0.5f) * display.x, (0.5f - ndc.y * 0.5f) * display.y};
}

static int pickJoint(const SkinnedModel& model, const Pose& pose, const glm::mat4& viewProj, ImVec2 display, ImVec2 mouse) {
	constexpr float kPickRadius = 14.f;
	int best = -1;
	float bestDistSq = kPickRadius * kPickRadius;
	for (size_t slot = 0; slot < model.joints.size(); slot++) {
		const auto p = projectToScreen(viewProj, glm::vec3(pose.globals[model.joints[slot]][3]), display);
		if (!p) continue;
		const float dx = p->x - mouse.x, dy = p->y - mouse.y;
		const float distSq = dx * dx + dy * dy;
		if (distSq < bestDistSq) {
			bestDistSq = distSq;
			best = static_cast<int>(slot);
		}
	}
	return best;
}

static void drawSkeletonOverlay(const SkinnedModel& model, const Pose& pose, const std::vector<int>& nodeToSlot, const glm::mat4& viewProj, int selectedSlot) {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const auto project = [&](size_t node) { return projectToScreen(viewProj, glm::vec3(pose.globals[node][3]), display); };
	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	for (size_t slot = 0; slot < model.joints.size(); slot++) {
		const size_t node = model.joints[slot];
		const auto p = project(node);
		if (!p) continue;
		const int parent = model.nodes[node].parent;
		if (parent >= 0 && nodeToSlot[parent] >= 0)
			if (const auto q = project(parent))
				drawList->AddLine(*q, *p, IM_COL32(255, 255, 255, 200), 2.f);
		const bool selected = static_cast<int>(slot) == selectedSlot;
		drawList->AddCircleFilled(*p, selected ? 6.f : 4.f, selected ? IM_COL32(255, 210, 0, 255) : IM_COL32(80, 200, 255, 255));
	}
}

// do not delete: this sample keeps the old hand-written api on purpose (GPUStructs.h, UNTYPED_SOURCES, cmdPushConstantsEXT, raw device addresses), so use the old features here as long as they compile
int main() {
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	SDL_Window* sdlWindow = BuildSDLWindow(true);
	const auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
	{
		Uint32 sdlExtensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
		auto ctx = mythril::CTXBuilder{}
		               .set_vulkan_cfg({.app_name = "Cool App Name",
						   .engine_name = "Cool Engine Name"})
		               .set_window_surface({.instanceExtensions = std::span(sdlExtensions, sdlExtensionCount),
						   .createFunc = [sdlWindow](VkInstance instance) {
				VkSurfaceKHR surface;
				SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
				return surface; },
						   .destroyFunc = [](VkInstance instance, VkSurfaceKHR surface) { SDL_Vulkan_DestroySurface(instance, surface, nullptr); }})
		               .with_default_swapchain({.width = initialWindowSize.width,
						   .height = initialWindowSize.height})
		               .with_ImGui({.windowInitFunction = [sdlWindow] { ImGui_ImplSDL3_InitForVulkan(sdlWindow); },
						   .windowDestroyFunction = [] { ImGui_ImplSDL3_Shutdown(); }})
		               .build();

		const SkinnedModel model = loadSkinnedModel(kDataDir / "RiggedFigure.glb");
		const size_t jointCount = model.joints.size();
		std::vector<int> nodeToSlot(model.nodes.size(), -1);
		for (size_t slot = 0; slot < jointCount; slot++)
			nodeToSlot[model.joints[slot]] = static_cast<int>(slot);

		mythril::Shader skinningShader = ctx->createShader({.path = kDataDir / shaders::Skinning::kSpirvFile,
			.debugName = "Skinning Shader"});
		mythril::GraphicsPipeline skinningPipeline = ctx->createGraphicsPipeline({.vertexShader = {skinningShader},
			.fragmentShader = {skinningShader},
			.topology = mythril::TopologyMode::TRIANGLE,
			.polygon = mythril::PolygonMode::FILL,
			.blend = mythril::BlendingMode::OFF,
			.cull = mythril::CullMode::BACK,
			.multisample = mythril::SampleCount::X1,
			.debugName = "Skinning Pipeline"});

		mythril::Buffer vertexBuffer = ctx->createBuffer({.size = sizeof(mythril::GPU::SkinnedVertex) * model.vertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = model.vertices,
			.debugName = "Skinned Vertex Buffer"});
		mythril::Buffer indexBuffer = ctx->createBuffer({.size = sizeof(uint32_t) * model.indices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
			.storage = mythril::StorageType::Device,
			.initialData = model.indices,
			.debugName = "Skinned Index Buffer"});
		mythril::Buffer jointMatrixBuffer = ctx->createBuffer({.size = sizeof(glm::mat4) * jointCount,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Joint Matrix Palette"});
		mythril::Buffer jointDualQuatBuffer = ctx->createBuffer({.size = sizeof(mythril::GPU::DualQuat) * jointCount,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Joint Dual Quaternion Palette"});
		mythril::Buffer frameDataBuffer = ctx->createBuffer({.size = sizeof(mythril::GPU::FrameData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Frame Data Buffer"});

		int skinningMode = mythril::GPU::kSkinLinear;
		int viewMode = mythril::GPU::kViewLit;
		int selectedSlot = 0;
		std::vector<glm::quat> rotationOffsets(jointCount, glm::quat(1.f, 0.f, 0.f, 0.f));
		Pose pose = evaluatePose(model, 0.f, rotationOffsets);
		mythril::GPU::FrameData frameData{};

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Color Texture"});
		const mythril::GraphTexture rgDepthTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .debugName = "Depth Texture"});
		const mythril::GraphBuffer rgJointMatrices = graph.importBuffer(jointMatrixBuffer);
		const mythril::GraphBuffer rgJointDualQuats = graph.importBuffer(jointDualQuatBuffer);
		const mythril::GraphBuffer rgFrameData = graph.importBuffer(frameDataBuffer);
		const mythril::GraphBuffer rgVertices = graph.importBuffer(vertexBuffer);
		const mythril::GraphBuffer rgIndices = graph.importBuffer(indexBuffer);

		struct SkinPass {
			mythril::GraphicsPipelineHandle pipeline;
			mythril::BufferHandle indexBuffer;
			uint32_t indexCount = 0;
			glm::mat4 model{};
			VkDeviceAddress vertices = 0;
			VkDeviceAddress frame = 0;
			VkDeviceAddress jointMatrices = 0;
			VkDeviceAddress jointDualQuats = 0;
			uint32_t skinningMode = 0;
			uint32_t viewMode = 0;
			uint32_t selectedJoint = 0;
		};
		const auto buildGraph = [&] {
			mythril::GraphBuffer jointMatrices = rgJointMatrices;
			mythril::GraphBuffer jointDualQuats = rgJointDualQuats;
			mythril::GraphBuffer frame = rgFrameData;
			graph.addTransferPass("per_frame_uploads", [&](mythril::TransferBuilder& b) {
				jointMatrices = b.update(jointMatrices, pose.jointMatrices);
				jointDualQuats = b.update(jointDualQuats, pose.jointDualQuats);
				frame = b.update(frame, frameData);
			});

			mythril::GraphTexture color = rgColorTarget;
			graph.addGraphicsPass<SkinPass>("skinned geometry", [&](mythril::GraphicsPassBuilder& b, SkinPass& d) {
				color = b.attachment({
					.texDesc = color,
					.clearValue = mythril::ClearValue::color(0.18f, 0.18f, 0.2f, 1.f),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.attachment({
					.texDesc = rgDepthTarget,
					.clearValue = mythril::ClearValue::depth(1.f, 0),
					.loadOp = mythril::LoadOp::CLEAR
				});
				b.dependency(jointMatrices, mythril::BufferAccess::ShaderRead);
				b.dependency(jointDualQuats, mythril::BufferAccess::ShaderRead);
				b.dependency(frame, mythril::BufferAccess::ShaderRead);
				b.dependency(rgVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgIndices, mythril::BufferAccess::IndexRead);
				d.pipeline = skinningPipeline.handle();
				d.indexBuffer = indexBuffer.handle();
				d.indexCount = static_cast<uint32_t>(model.indices.size());
				d.model = pose.globals[model.meshNode];
				d.vertices = ctx->gpuAddress(vertexBuffer);
				d.frame = ctx->gpuAddress(frameDataBuffer);
				d.jointMatrices = ctx->gpuAddress(jointMatrixBuffer);
				d.jointDualQuats = ctx->gpuAddress(jointDualQuatBuffer);
				d.skinningMode = static_cast<uint32_t>(skinningMode);
				d.viewMode = static_cast<uint32_t>(viewMode);
				d.selectedJoint = static_cast<uint32_t>(selectedSlot); }, [](const SkinPass& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
				const mythril::GPU::SkinPushConstant push = {
					.model = d.model,
					.vertices = d.vertices,
					.frame = d.frame,
					.jointMatrices = d.jointMatrices,
					.jointDualQuats = d.jointDualQuats,
					.skinningMode = d.skinningMode,
					.viewMode = d.viewMode,
					.selectedJoint = d.selectedJoint
				};
				cmd.cmdPushConstantsEXT(push);
				cmd.cmdBindIndexBuffer(d.indexBuffer);
				cmd.cmdDrawIndexed(d.indexCount); });

			graph.addGraphicsPass("gui", [&](mythril::GraphicsPassBuilder& b, mythril::NoPassData&) { color = b.attachment({.texDesc = color,
																															   .loadOp = mythril::LoadOp::LOAD,
																															   .storeOp = mythril::StoreOp::STORE})
				                                                                                                  .texture; }, [](const mythril::NoPassData&, mythril::GraphicsCmd& cmd) { cmd.cmdDrawImGui(); });

			graph.addTransferPass("presentation", [&](mythril::TransferBuilder& b) {
				b.blit(color, graph.backBuffer());
			});
		};

		glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
		for (const mythril::GPU::SkinnedVertex& v: model.vertices) {
			const auto p = glm::vec3(pose.globals[model.meshNode] * glm::vec4(v.position, 1.f));
			boundsMin = glm::min(boundsMin, p);
			boundsMax = glm::max(boundsMax, p);
		}
		const glm::vec3 orbitTarget = (boundsMin + boundsMax) * 0.5f;
		float orbitDistance = glm::length(boundsMax - boundsMin) * 1.2f;
		float orbitYaw = 30.f, orbitPitch = 10.f;

		bool playing = true;
		bool showSkeleton = true;
		float playbackSpeed = 1.f;
		float animationTime = 0.f;
		bool clickPending = false;
		bool pickRequested = false;
		ImVec2 pressPos{};

		auto lastTime = std::chrono::steady_clock::now();
		bool quit = false;
		while (!quit) {
			const auto now = std::chrono::steady_clock::now();
			const float deltaTime = std::chrono::duration<float>(now - lastTime).count();
			lastTime = now;

			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				ImGui_ImplSDL3_ProcessEvent(&e);
				if (e.type == SDL_EVENT_QUIT) quit = true;
				if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_Q) quit = true;
				const bool mouseFree = !ImGui::GetIO().WantCaptureMouse && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing();
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
					clickPending = mouseFree;
					pressPos = {e.button.x, e.button.y};
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION && clickPending) {
					const float dx = e.motion.x - pressPos.x, dy = e.motion.y - pressPos.y;
					if (dx * dx + dy * dy > 16.f) clickPending = false;
				}
				if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && clickPending) {
					pickRequested = true;
					clickPending = false;
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION && mouseFree && (e.motion.state & SDL_BUTTON_LMASK)) {
					orbitYaw += e.motion.xrel * 0.4f;
					orbitPitch = glm::clamp(orbitPitch + e.motion.yrel * 0.4f, -85.f, 85.f);
				}
				if (e.type == SDL_EVENT_MOUSE_WHEEL && mouseFree)
					orbitDistance = glm::max(0.2f, orbitDistance * (1.f - e.wheel.y * 0.1f));
			}

			const auto currentWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({.width = currentWindowSize.width,
					.height = currentWindowSize.height});
			}

			if (playing && model.duration > 0.f)
				animationTime = std::fmod(animationTime + deltaTime * playbackSpeed, model.duration);

			const glm::vec3 orbitDir = {
				std::cos(glm::radians(orbitPitch)) * std::sin(glm::radians(orbitYaw)),
				std::sin(glm::radians(orbitPitch)),
				std::cos(glm::radians(orbitPitch)) * std::cos(glm::radians(orbitYaw))};
			const glm::vec3 cameraPosition = orbitTarget + orbitDir * orbitDistance;
			const float aspect = static_cast<float>(currentWindowSize.width) / static_cast<float>(currentWindowSize.height);
			const glm::mat4 view = glm::lookAt(cameraPosition, orbitTarget, glm::vec3(0, 1, 0));
			const glm::mat4 proj = glm::perspective(glm::radians(45.f), aspect, 0.01f, 100.f);
			const glm::mat4 viewProj = proj * view;
			pose = evaluatePose(model, animationTime, rotationOffsets);

			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();

			ImGui::Begin("Skinning");
			ImGui::Text("RiggedFigure by Cesium, CC-BY 4.0.");
			ImGui::TextLinkOpenURL("Khronos glTF Sample Assets", "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/RiggedFigure");
			ImGui::SeparatorText("Animation");
			ImGui::Checkbox("Play", &playing);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##speed", &playbackSpeed, 0.f, 3.f, "speed %.2fx");
			ImGui::SliderFloat("Time", &animationTime, 0.f, model.duration, "%.3f s");

			ImGui::SeparatorText("Skinning");
			ImGui::RadioButton("Linear Blend", &skinningMode, mythril::GPU::kSkinLinear);
			ImGui::SameLine();
			ImGui::RadioButton("Dual Quaternion", &skinningMode, mythril::GPU::kSkinDualQuat);
			ImGui::SameLine();
			ImGui::RadioButton("Bind Pose", &skinningMode, mythril::GPU::kSkinNone);

			ImGui::SeparatorText("View");
			ImGui::RadioButton("Lit", &viewMode, mythril::GPU::kViewLit);
			ImGui::SameLine();
			ImGui::RadioButton("Selected Weights", &viewMode, mythril::GPU::kViewWeights);
			ImGui::SameLine();
			ImGui::RadioButton("Influences", &viewMode, mythril::GPU::kViewInfluence);
			ImGui::Checkbox("Show Skeleton", &showSkeleton);

			ImGui::SeparatorText("Joints");
			if (ImGui::Button("Reset Joint"))
				rotationOffsets[selectedSlot] = glm::quat(1.f, 0.f, 0.f, 0.f);
			ImGui::SameLine();
			if (ImGui::Button("Reset All"))
				std::fill(rotationOffsets.begin(), rotationOffsets.end(), glm::quat(1.f, 0.f, 0.f, 0.f));
			ImGui::End();

			const ImVec2 display = ImGui::GetIO().DisplaySize;
			if (pickRequested) {
				pickRequested = false;
				const int picked = pickJoint(model, pose, viewProj, display, pressPos);
				if (picked >= 0) selectedSlot = picked;
			}

			ImGuizmo::BeginFrame();
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
			ImGuizmo::SetRect(0, 0, display.x, display.y);
			const glm::mat4& selectedGlobal = pose.globals[model.joints[selectedSlot]];
			const glm::quat globalRotation = glm::normalize(glm::quat_cast(glm::mat3(
				glm::normalize(glm::vec3(selectedGlobal[0])),
				glm::normalize(glm::vec3(selectedGlobal[1])),
				glm::normalize(glm::vec3(selectedGlobal[2])))));
			glm::mat4 gizmoMatrix = glm::translate(glm::mat4(1.f), glm::vec3(selectedGlobal[3])) * glm::mat4_cast(globalRotation);
			ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::ROTATE, ImGuizmo::WORLD, glm::value_ptr(gizmoMatrix));
			if (ImGuizmo::IsUsing()) {
				const glm::quat newRotation = glm::normalize(glm::quat_cast(glm::mat3(gizmoMatrix)));
				glm::quat& offset = rotationOffsets[selectedSlot];
				offset = glm::normalize(offset * glm::inverse(globalRotation) * newRotation);
				pose = evaluatePose(model, animationTime, rotationOffsets);
			}

			frameData = mythril::GPU::FrameData{
				.viewProj = viewProj,
				.lightDir = glm::normalize(orbitDir + glm::vec3(0.f, 1.f, 0.f))};

			if (showSkeleton)
				drawSkeletonOverlay(model, pose, nodeToSlot, viewProj, selectedSlot);

			graph.reset();
			buildGraph();
			graph.compile(*ctx);
			graph.execute(*ctx);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
