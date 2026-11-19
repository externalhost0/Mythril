//
// Created by Hayden Rivas on 11/17/26.
//
#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "mythril/RenderGraphBuilder.h"

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/ext/matrix_transform.hpp"
#include "glm/ext/matrix_clip_space.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <optional>

#include "GPUStructs.h"
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
	size_t node;
	fastgltf::AnimationPath path;
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
	std::vector<uint32_t> jointVertexCounts;

	std::vector<Channel> channels;
	float duration = 0.f;
};

static SkinnedModel loadSkinnedModel(const std::filesystem::path& filepath) {
	auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
	ASSERT_MSG(data.error() == fastgltf::Error::None, "Failed to read {}", filepath.string());
	fastgltf::Parser parser;
	auto asset = parser.loadGltfBinary(data.get(), filepath.parent_path(), fastgltf::Options::LoadExternalBuffers);
	ASSERT_MSG(asset.error() == fastgltf::Error::None, "Failed to parse {}", filepath.string());
	const fastgltf::Asset& gltf = asset.get();
	ASSERT_MSG(gltf.skins.size() == 1, "Sample expects exactly one skin");

	SkinnedModel model;

	model.nodes.resize(gltf.nodes.size());
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		const fastgltf::Node& src = gltf.nodes[i];
		Node& dst = model.nodes[i];
		dst.name = src.name;
		dst.children.assign(src.children.begin(), src.children.end());
		for (size_t child : src.children)
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
			}
		}, src.transform);
		if (src.meshIndex.has_value())
			model.meshNode = i;
	}
	const auto visit = [&](auto& self, size_t node) -> void {
		model.parentFirstOrder.push_back(node);
		for (size_t child : model.nodes[node].children)
			self(self, child);
	};
	for (size_t root : gltf.scenes[gltf.defaultScene.value_or(0)].nodeIndices)
		visit(visit, root);

	const fastgltf::Skin& skin = gltf.skins[0];
	model.joints.assign(skin.joints.begin(), skin.joints.end());
	model.inverseBindMatrices.resize(model.joints.size(), glm::mat4(1.f));
	if (skin.inverseBindMatrices.has_value())
		fastgltf::copyFromAccessor<glm::mat4>(gltf, gltf.accessors[skin.inverseBindMatrices.value()], model.inverseBindMatrices.data());

	const fastgltf::Primitive& primitive = gltf.meshes[gltf.nodes[model.meshNode].meshIndex.value()].primitives[0];
	const auto accessorFor = [&](const char* name) -> const fastgltf::Accessor& {
		const auto* attribute = primitive.findAttribute(name);
		ASSERT_MSG(attribute != primitive.attributes.end(), "Mesh is missing {}", name);
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

	model.jointVertexCounts.resize(model.joints.size(), 0);
	for (const mythril::GPU::SkinnedVertex& v : model.vertices)
		for (int i = 0; i < 4; i++)
			if (v.weights[i] > 0.f)
				model.jointVertexCounts[v.joints[i]]++;

	if (!gltf.animations.empty()) {
		const fastgltf::Animation& animation = gltf.animations[0];
		for (const fastgltf::AnimationChannel& srcChannel : animation.channels) {
			if (!srcChannel.nodeIndex.has_value() || srcChannel.path == fastgltf::AnimationPath::Weights)
				continue;
			const fastgltf::AnimationSampler& sampler = animation.samplers[srcChannel.samplerIndex];
			ASSERT_MSG(sampler.interpolation == fastgltf::AnimationInterpolation::Linear, "Sample only handles linear keyframes");
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

static Pose evaluatePose(const SkinnedModel& model, float time, std::span<const glm::vec3> rotationOffsets) {
	Pose pose;
	std::vector<Node> locals = model.nodes;
	for (const Channel& channel : model.channels) {
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
		locals[model.joints[slot]].rotation *= glm::quat(glm::radians(rotationOffsets[slot]));

	pose.globals.resize(model.nodes.size());
	for (size_t n : model.parentFirstOrder) {
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
			.dual = {dual.x, dual.y, dual.z, dual.w}
		};
	}
	return pose;
}

static void drawJointTree(const SkinnedModel& model, const std::vector<int>& nodeToSlot, size_t node, int& selectedSlot) {
	const int slot = nodeToSlot[node];
	bool hasJointChildren = false;
	for (size_t child : model.nodes[node].children)
		hasJointChildren |= nodeToSlot[child] >= 0;
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (!hasJointChildren) flags |= ImGuiTreeNodeFlags_Leaf;
	if (slot == selectedSlot) flags |= ImGuiTreeNodeFlags_Selected;
	const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(node), flags, "[%d] %s", slot, model.nodes[node].name.c_str());
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		selectedSlot = slot;
	if (open) {
		for (size_t child : model.nodes[node].children)
			if (nodeToSlot[child] >= 0)
				drawJointTree(model, nodeToSlot, child, selectedSlot);
		ImGui::TreePop();
	}
}

static void drawSkeletonOverlay(const SkinnedModel& model, const Pose& pose, const std::vector<int>& nodeToSlot, const glm::mat4& viewProj, int selectedSlot) {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const auto project = [&](size_t node, ImVec2& out) {
		const glm::vec4 clip = viewProj * glm::vec4(glm::vec3(pose.globals[node][3]), 1.f);
		if (clip.w <= 0.f) return false;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		out = {(ndc.x * 0.5f + 0.5f) * display.x, (0.5f - ndc.y * 0.5f) * display.y};
		return true;
	};
	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	for (size_t slot = 0; slot < model.joints.size(); slot++) {
		const size_t node = model.joints[slot];
		ImVec2 p;
		if (!project(node, p)) continue;
		const int parent = model.nodes[node].parent;
		ImVec2 q;
		if (parent >= 0 && nodeToSlot[parent] >= 0 && project(parent, q))
			drawList->AddLine(q, p, IM_COL32(255, 255, 255, 200), 2.f);
		const bool selected = static_cast<int>(slot) == selectedSlot;
		drawList->AddCircleFilled(p, selected ? 6.f : 4.f, selected ? IM_COL32(255, 210, 0, 255) : IM_COL32(80, 200, 255, 255));
	}
}

int main() {
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	static const std::vector<std::string> slang_searchpaths = {
		"../../include/",
		"../include/",
		kDataDir.string()
	};
	SDL_Window* sdlWindow = BuildSDLWindow(true);
	const auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
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
		.set_slang_cfg({
			.searchpaths = slang_searchpaths
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

		const SkinnedModel model = loadSkinnedModel(kDataDir / "RiggedFigure.glb");
		const size_t jointCount = model.joints.size();
		std::vector<int> nodeToSlot(model.nodes.size(), -1);
		for (size_t slot = 0; slot < jointCount; slot++)
			nodeToSlot[model.joints[slot]] = static_cast<int>(slot);

		mythril::Shader skinningShader = ctx->createShader({
			.source = kDataDir / "Skinning.slang",
			.debugName = "Skinning Shader"
		});
		mythril::GraphicsPipeline skinningPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {skinningShader},
			.fragmentShader = {skinningShader},
			.topology = mythril::TopologyMode::TRIANGLE,
			.polygon = mythril::PolygonMode::FILL,
			.blend = mythril::BlendingMode::OFF,
			.cull = mythril::CullMode::BACK,
			.multisample = mythril::SampleCount::X1,
			.debugName = "Skinning Pipeline"
		});

		mythril::Buffer vertexBuffer = ctx->createBuffer({
			.size = sizeof(mythril::GPU::SkinnedVertex) * model.vertices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = model.vertices.data(),
			.debugName = "Skinned Vertex Buffer"
		});
		mythril::Buffer indexBuffer = ctx->createBuffer({
			.size = sizeof(uint32_t) * model.indices.size(),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
			.storage = mythril::StorageType::Device,
			.initialData = model.indices.data(),
			.debugName = "Skinned Index Buffer"
		});
		mythril::Buffer jointMatrixBuffer = ctx->createBuffer({
			.size = sizeof(glm::mat4) * jointCount,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Joint Matrix Palette"
		});
		mythril::Buffer jointDualQuatBuffer = ctx->createBuffer({
			.size = sizeof(mythril::GPU::DualQuat) * jointCount,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Joint Dual Quaternion Palette"
		});
		mythril::Buffer frameDataBuffer = ctx->createBuffer({
			.size = sizeof(mythril::GPU::FrameData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "Frame Data Buffer"
		});

		int skinningMode = mythril::GPU::kSkinLinear;
		int viewMode = mythril::GPU::kViewLit;
		int selectedSlot = 0;
		std::vector<glm::vec3> rotationOffsets(jointCount, glm::vec3(0.f));
		Pose pose = evaluatePose(model, 0.f, rotationOffsets);
		mythril::GPU::FrameData frameData{};

		mythril::RenderGraph graph;
		const mythril::GraphTexture rgColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Color Texture"});
		const mythril::GraphTexture rgDepthTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .debugName = "Depth Texture"});
		graph.addIntermediate("per_frame_uploads")
		.update(jointMatrixBuffer, [&] { return mythril::UploadData{pose.jointMatrices.data(), pose.jointMatrices.size() * sizeof(glm::mat4)}; })
		.update(jointDualQuatBuffer, [&] { return mythril::UploadData{pose.jointDualQuats.data(), pose.jointDualQuats.size() * sizeof(mythril::GPU::DualQuat)}; })
		.update(frameDataBuffer, [&] { return mythril::UploadData{&frameData, sizeof(frameData)}; })
		.finish();

		graph.addGraphicsPass("skinned geometry")
		.attachment({
			.texDesc = rgColorTarget,
			.clearValue = mythril::ClearValue::color(0.18f, 0.18f, 0.2f, 1.f),
			.loadOp = mythril::LoadOp::CLEAR,
			.storeOp = mythril::StoreOp::STORE
		})
		.attachment({
			.texDesc = rgDepthTarget,
			.clearValue = mythril::ClearValue::depth(1.f, 0),
			.loadOp = mythril::LoadOp::CLEAR
		})
		.dependency(jointMatrixBuffer, mythril::BufferAccess::ShaderRead)
		.dependency(jointDualQuatBuffer, mythril::BufferAccess::ShaderRead)
		.dependency(frameDataBuffer, mythril::BufferAccess::ShaderRead)
		.execute([&](mythril::CommandBuffer& cmd) {
			cmd.cmdBindGraphicsPipeline(skinningPipeline);
			cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
			const mythril::GPU::SkinPushConstant push = {
				.model = pose.globals[model.meshNode],
				.vertices = ctx->gpuAddress(vertexBuffer),
				.frame = ctx->gpuAddress(frameDataBuffer),
				.jointMatrices = ctx->gpuAddress(jointMatrixBuffer),
				.jointDualQuats = ctx->gpuAddress(jointDualQuatBuffer),
				.skinningMode = static_cast<uint32_t>(skinningMode),
				.viewMode = static_cast<uint32_t>(viewMode),
				.selectedJoint = static_cast<uint32_t>(selectedSlot)
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(indexBuffer);
			cmd.cmdDrawIndexed(model.indices.size());
		});

		graph.addGraphicsPass("gui")
		.attachment({
			.texDesc = {rgColorTarget},
			.loadOp = mythril::LoadOp::LOAD,
			.storeOp = mythril::StoreOp::STORE
		})
		.execute([&](mythril::CommandBuffer& cmd) {
			cmd.cmdDrawImGui();
		});

		graph.addIntermediate("presentation")
		.blit(rgColorTarget, graph.backBuffer())
		.finish();

		graph.compile(*ctx);

		glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
		for (const mythril::GPU::SkinnedVertex& v : model.vertices) {
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
				const bool mouseFree = !ImGui::GetIO().WantCaptureMouse;
				if (e.type == SDL_EVENT_MOUSE_MOTION && mouseFree && (e.motion.state & SDL_BUTTON_LMASK)) {
					orbitYaw += e.motion.xrel * 0.4f;
					orbitPitch = glm::clamp(orbitPitch + e.motion.yrel * 0.4f, -85.f, 85.f);
				}
				if (e.type == SDL_EVENT_MOUSE_WHEEL && mouseFree)
					orbitDistance = glm::max(0.2f, orbitDistance * (1.f - e.wheel.y * 0.1f));
			}

			const auto currentWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({
					.width = currentWindowSize.width,
					.height = currentWindowSize.height
				});
			}

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
			ImGui::TextWrapped("Twist an arm joint around Y by ~150 degrees, then compare Linear Blend with Dual Quaternion.");
			if (ImGui::BeginChild("joint tree", ImVec2(0, 220), ImGuiChildFlags_Borders))
				drawJointTree(model, nodeToSlot, model.joints[0], selectedSlot);
			ImGui::EndChild();

			const size_t selectedNode = model.joints[selectedSlot];
			const auto selectedWorld = glm::vec3(pose.globals[selectedNode][3]);
			ImGui::Text("%s", model.nodes[selectedNode].name.c_str());
			ImGui::TextDisabled("slot %d | node %zu | influences %u vertices", selectedSlot, selectedNode, model.jointVertexCounts[selectedSlot]);
			ImGui::TextDisabled("world (%.3f, %.3f, %.3f)", selectedWorld.x, selectedWorld.y, selectedWorld.z);
			ImGui::SliderFloat3("Rotation Offset", &rotationOffsets[selectedSlot].x, -180.f, 180.f, "%.0f deg");
			if (ImGui::Button("Reset Joint"))
				rotationOffsets[selectedSlot] = glm::vec3(0.f);
			ImGui::SameLine();
			if (ImGui::Button("Reset All"))
				std::fill(rotationOffsets.begin(), rotationOffsets.end(), glm::vec3(0.f));
			ImGui::End();

			if (playing && model.duration > 0.f)
				animationTime = std::fmod(animationTime + deltaTime * playbackSpeed, model.duration);
			pose = evaluatePose(model, animationTime, rotationOffsets);

			const glm::vec3 orbitDir = {
				std::cos(glm::radians(orbitPitch)) * std::sin(glm::radians(orbitYaw)),
				std::sin(glm::radians(orbitPitch)),
				std::cos(glm::radians(orbitPitch)) * std::cos(glm::radians(orbitYaw))
			};
			const glm::vec3 cameraPosition = orbitTarget + orbitDir * orbitDistance;
			const float aspect = static_cast<float>(currentWindowSize.width) / static_cast<float>(currentWindowSize.height);
			const glm::mat4 viewProj = glm::perspective(glm::radians(45.f), aspect, 0.01f, 100.f) * glm::lookAt(cameraPosition, orbitTarget, glm::vec3(0, 1, 0));
			frameData = mythril::GPU::FrameData {
				.viewProj = viewProj,
				.lightDir = glm::normalize(orbitDir + glm::vec3(0.f, 1.f, 0.f))
			};

			if (showSkeleton)
				drawSkeletonOverlay(model, pose, nodeToSlot, viewProj, selectedSlot);

			mythril::CommandBuffer& cmd = ctx->acquireCommand(mythril::CommandBuffer::Type::Graphics);
			graph.execute(cmd);
			ctx->submitCommand(cmd);
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
