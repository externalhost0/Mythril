#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <mythril/CTX.h>
#include <mythril/RenderGraphBuilder.h>
#include <glm/ext/matrix_transform.hpp>

#include "Meshes.h"
#include "Scene.shadergen.h"

// instance order in the TLAS is ground, then the trees, then the leaves
constexpr float kFloorHeight = -1.f;
using shaders::Scene::kTreeCount;
constexpr uint32_t kLeafCount = 96;
constexpr uint32_t kFirstLeafInstance = 1 + kTreeCount;

struct SceneObject {
	mythril::Buffer vertices;
	mythril::Buffer indices;
	mythril::AccelerationStructure blas;
	uint32_t vertexCount = 0;
};

// the wind pass bends rest into bent.vertices every frame, then bent.blas is refit
struct Tree {
	SceneObject bent;
	mythril::Buffer rest;
	glm::vec3 root{};
	float height{};
	float phase{};

	glm::vec3 crown() const { return root + glm::vec3(0.f, 0.75f * height, 0.f); }
};

struct Scene {
	SceneObject ground;
	SceneObject leaf;
	std::array<Tree, kTreeCount> trees;
	mythril::Buffer geometries;
	mythril::Buffer instances;
	mythril::Buffer bounds;
	mythril::AccelerationStructure tlas;
	uint32_t instanceCount = 0;
};

// graph handles for every scene resource, imported once and reused by each frame's graph
struct SceneGraphResources {
	mythril::GraphBuffer geometries, instances, bounds;
	mythril::GraphBuffer groundVertices, groundIndices, leafVertices, leafIndices;
	mythril::GraphAccelerationStructure groundBlas, leafBlas, tlas;
	std::array<mythril::GraphBuffer, kTreeCount> treeRest, treeBent, treeIndices;
	std::array<mythril::GraphAccelerationStructure, kTreeCount> treeBlas;
};

namespace detail {
	constexpr mythril::BufferUsageBits kGeometryUsage = mythril::BufferUsageBits::BufferUsageBits_Storage | mythril::BufferUsageBits_AccelerationStructureInputReadOnly;

	inline SceneObject createObject(mythril::CTX& ctx, const Mesh& mesh, const std::string& name, const bool refits) {
		SceneObject object;
		object.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
		object.vertices = ctx.createBuffer({.size = sizeof(Vertex) * mesh.vertices.size(),
			.usage = kGeometryUsage,
			.storage = mythril::StorageType::Device,
			.initialData = mesh.vertices,
			.debugName = (name + " Vertices").c_str()});
		object.indices = ctx.createBuffer({.size = sizeof(uint32_t) * mesh.indices.size(),
			.usage = kGeometryUsage,
			.storage = mythril::StorageType::Device,
			.initialData = mesh.indices,
			.debugName = (name + " Indices").c_str()});
		object.blas = ctx.createAccelerationStructure({.type = mythril::AccelerationStructureSpec::Type::BLAS,
			.geometryType = mythril::AccelerationStructureSpec::GeometryType::Triangle,
			.geometryFlags = VK_GEOMETRY_OPAQUE_BIT_KHR,
			.vertexBuffer = object.vertices.handle(),
			.numVertices = object.vertexCount,
			.vertexStride = sizeof(Vertex),
			.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
			.indexBuffer = object.indices.handle(),
			.indexType = VK_INDEX_TYPE_UINT32,
			.buildRange = {.primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3)},
			.buildFlags = refits ? VkBuildAccelerationStructureFlagsKHR{VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR} : VkBuildAccelerationStructureFlagsKHR{VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR},
			.debugName = (name + " BLAS").c_str()});
		return object;
	}

	inline Tree createTree(mythril::CTX& ctx, const uint32_t seed, const glm::vec3 root, const float scale, const float phase, const std::string& name) {
		Mesh placed;
		appendMesh(placed, buildTreeMesh({.seed = seed}), glm::scale(glm::translate(glm::mat4(1.f), root), glm::vec3(scale)));
		float top = root.y;
		for (const Vertex& v: placed.vertices)
			top = std::max(top, v.position.y);
		return {
			.bent = createObject(ctx, placed, name, true),
			.rest = ctx.createBuffer({.size = sizeof(Vertex) * placed.vertices.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
				.storage = mythril::StorageType::Device,
				.initialData = placed.vertices,
				.debugName = (name + " Rest Vertices").c_str()}),
			.root = root,
			.height = top - root.y,
			.phase = phase,
		};
	}
}

inline Scene createScene(mythril::CTX& ctx) {
	Scene scene;
	scene.ground = detail::createObject(ctx, buildGroundMesh(12.f, kFloorHeight), "Ground", false);
	scene.leaf = detail::createObject(ctx, buildLeafMesh(), "Leaf", false);
	scene.trees = {
		detail::createTree(ctx, 7, {3.5f, kFloorHeight, -1.f}, 0.7f, 0.f, "Tree 0"),
		detail::createTree(ctx, 21, {-3.5f, kFloorHeight, -2.5f}, 0.8f, 1.9f, "Tree 1"),
	};

	const auto geometryOf = [&](const SceneObject& object, const glm::vec3 albedo) {
		return shaders::Geometry{
			.vertices = ctx.gpuAddress(object.vertices),
			.indices = ctx.gpuAddress(object.indices),
			.albedo = glm::vec4(albedo, 1.f),
			.vertexCount = object.vertexCount,
		};
	};
	std::vector<shaders::Geometry> geometries;
	geometries.push_back(geometryOf(scene.ground, glm::vec3(0.8f)));
	for (const Tree& tree: scene.trees)
		geometries.push_back(geometryOf(tree.bent, glm::vec3(0.55f, 0.45f, 0.38f)));
	geometries.push_back(geometryOf(scene.leaf, glm::vec3(0.95f, 0.5f, 0.12f)));
	scene.geometries = ctx.createBuffer({.size = sizeof(shaders::Geometry) * geometries.size(),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.initialData = geometries,
		.debugName = "Geometry Table"});

	static_assert(sizeof(shaders::Instance) == sizeof(VkAccelerationStructureInstanceKHR));
	const auto instanceOf = [&](const SceneObject& object, const uint32_t geometry) {
		return shaders::Instance{
			.row0 = {1.f, 0.f, 0.f, 0.f},
			.row1 = {0.f, 1.f, 0.f, 0.f},
			.row2 = {0.f, 0.f, 1.f, 0.f},
			.customIndexAndMask = geometry | (0xFFu << 24),
			.sbtOffsetAndFlags = 0,
			.blas = ctx.gpuAddress(object.blas),
		};
	};
	std::vector<shaders::Instance> instances;
	instances.push_back(instanceOf(scene.ground, shaders::Scene::kGeometryGround));
	for (uint32_t i = 0; i < kTreeCount; i++)
		instances.push_back(instanceOf(scene.trees[i].bent, shaders::Scene::kGeometryTreeFirst + i));
	for (uint32_t i = 0; i < kLeafCount; i++)
		instances.push_back(instanceOf(scene.leaf, shaders::Scene::kGeometryLeaf));
	scene.instanceCount = static_cast<uint32_t>(instances.size());
	scene.instances = ctx.createBuffer({.size = sizeof(shaders::Instance) * instances.size(),
		.usage = detail::kGeometryUsage,
		.storage = mythril::StorageType::Device,
		.initialData = instances,
		.debugName = "TLAS Instances"});
	scene.bounds = ctx.createBuffer({.size = sizeof(shaders::Bounds) * scene.instanceCount,
		.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.debugName = "Instance Bounds"});
	scene.tlas = ctx.createAccelerationStructure({.type = mythril::AccelerationStructureSpec::Type::TLAS,
		.geometryType = mythril::AccelerationStructureSpec::GeometryType::Instances,
		.instancesBuffer = scene.instances.handle(),
		.buildRange = {.primitiveCount = scene.instanceCount},
		.buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
		.debugName = "TLAS"});
	return scene;
}

// every buffer the geometry table points at, with this frame's bent tree vertices
inline std::array<mythril::GraphBuffer, 4 + 2 * kTreeCount> GeometryBuffers(const SceneGraphResources& rg, const std::array<mythril::GraphBuffer, kTreeCount>& bent) {
	std::array<mythril::GraphBuffer, 4 + 2 * kTreeCount> buffers = {rg.groundVertices, rg.groundIndices, rg.leafVertices, rg.leafIndices};
	for (uint32_t i = 0; i < kTreeCount; i++) {
		buffers[4 + 2 * i] = bent[i];
		buffers[5 + 2 * i] = rg.treeIndices[i];
	}
	return buffers;
}

inline SceneGraphResources importScene(mythril::RenderGraph& graph, const Scene& scene) {
	SceneGraphResources rg = {
		.geometries = graph.importBuffer(scene.geometries),
		.instances = graph.importBuffer(scene.instances),
		.bounds = graph.importBuffer(scene.bounds),
		.groundVertices = graph.importBuffer(scene.ground.vertices),
		.groundIndices = graph.importBuffer(scene.ground.indices),
		.leafVertices = graph.importBuffer(scene.leaf.vertices),
		.leafIndices = graph.importBuffer(scene.leaf.indices),
		.groundBlas = graph.importAccelerationStructure(scene.ground.blas),
		.leafBlas = graph.importAccelerationStructure(scene.leaf.blas),
		.tlas = graph.importAccelerationStructure(scene.tlas),
	};
	for (uint32_t i = 0; i < kTreeCount; i++) {
		rg.treeRest[i] = graph.importBuffer(scene.trees[i].rest);
		rg.treeBent[i] = graph.importBuffer(scene.trees[i].bent.vertices);
		rg.treeIndices[i] = graph.importBuffer(scene.trees[i].bent.indices);
		rg.treeBlas[i] = graph.importAccelerationStructure(scene.trees[i].bent.blas);
	}
	return rg;
}
