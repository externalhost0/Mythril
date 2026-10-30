#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

struct Vertex {
	glm::vec3 position;
};
struct Mesh {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
};

// rebases src's indices onto the end of dst's vertices
inline void appendMesh(Mesh& dst, const Mesh& src, const glm::mat4& transform = glm::mat4(1.f)) {
	const auto base = static_cast<uint32_t>(dst.vertices.size());
	dst.vertices.reserve(dst.vertices.size() + src.vertices.size());
	for (const Vertex& v: src.vertices) {
		dst.vertices.push_back({glm::vec3(transform * glm::vec4(v.position, 1.f))});
	}
	dst.indices.reserve(dst.indices.size() + src.indices.size());
	for (const uint32_t index: src.indices) {
		dst.indices.push_back(base + index);
	}
}

struct TreeSpec {
	uint32_t seed = 7;
	float trunkLength = 2.6f;
	float trunkRadius = 0.32f;
	// recursion levels below the trunk
	uint32_t maxDepth = 4;
	uint32_t ringsPerBranch = 6;
	uint32_t radialSegments = 10;
	uint32_t sideBranchesMin = 1;
	uint32_t sideBranchesMax = 3;
	// side branches sprout between these fractions of their parent's length
	float sideAttachStart = 0.4f;
	float sideAttachEnd = 0.9f;
	float leaderLengthScale = 0.72f;
	float sideLengthScale = 0.75f;
	float childRadiusScale = 0.6f;
	// tip radius relative to the branch's base radius
	float taper = 0.65f;
	// taper for the outermost branches so they end as twigs instead of stubs
	float twigTaper = 0.12f;
	float leaderAngleDeg = 15.f;
	float sideAngleDeg = 55.f;
	// random per ring wobble, gives the gnarled look
	float bendDeg = 12.f;
	// pulls every segment toward +y so branches reach up instead of out
	float upwardBias = 0.06f;
};

namespace detail {
	struct TreeBuilder {
		const TreeSpec& spec;
		Mesh& mesh;
		std::mt19937 rng;

		float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
		uint32_t uniformInt(uint32_t lo, uint32_t hi) { return std::uniform_int_distribution<uint32_t>(lo, hi)(rng); }

		static glm::vec3 anyPerpendicular(const glm::vec3& d) {
			return glm::normalize(glm::cross(d, glm::abs(d.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
		}
		glm::vec3 tilt(const glm::vec3& d, float maxDeg) {
			const glm::vec3 axis = glm::angleAxis(uniform(0.f, glm::two_pi<float>()), d) * anyPerpendicular(d);
			return glm::normalize(glm::angleAxis(glm::radians(uniform(0.f, maxDeg)), axis) * d);
		}

		struct Ring {
			glm::vec3 center;
			glm::vec3 direction;
			glm::vec3 normal;
			float radius;
		};

		void emitTube(const std::vector<Ring>& rings) {
			const uint32_t segs = spec.radialSegments;
			const auto first = static_cast<uint32_t>(mesh.vertices.size());
			for (const Ring& ring: rings) {
				const glm::vec3 bitangent = glm::cross(ring.normal, ring.direction);
				for (uint32_t i = 0; i < segs; i++) {
					const float a = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segs);
					mesh.vertices.push_back({ring.center + ring.radius * (glm::cos(a) * ring.normal + glm::sin(a) * bitangent)});
				}
			}
			for (uint32_t r = 0; r + 1 < rings.size(); r++) {
				const uint32_t lower = first + r * segs;
				const uint32_t upper = lower + segs;
				for (uint32_t i = 0; i < segs; i++) {
					const uint32_t j = (i + 1) % segs;
					mesh.indices.insert(mesh.indices.end(), {lower + i, upper + i, lower + j, lower + j, upper + i, upper + j});
				}
			}
			// close the tip into a point so shadow rays can't slip into the tube
			const Ring& tip = rings.back();
			const auto apex = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.push_back({tip.center + tip.direction * tip.radius * 2.f});
			const uint32_t last = first + static_cast<uint32_t>(rings.size() - 1) * segs;
			for (uint32_t i = 0; i < segs; i++) {
				mesh.indices.insert(mesh.indices.end(), {last + i, apex, last + (i + 1) % segs});
			}
		}

		void growBranch(glm::vec3 position, glm::vec3 direction, float length, float radius, uint32_t depth) {
			std::vector<Ring> rings;
			rings.reserve(spec.ringsPerBranch + 1);
			glm::vec3 normal = anyPerpendicular(direction);
			const float step = length / static_cast<float>(spec.ringsPerBranch);
			const float taper = depth >= spec.maxDepth ? spec.twigTaper : spec.taper;
			for (uint32_t s = 0; s <= spec.ringsPerBranch; s++) {
				const float t = static_cast<float>(s) / static_cast<float>(spec.ringsPerBranch);
				rings.push_back({position, direction, normal, radius * glm::mix(1.f, taper, t)});
				if (s == spec.ringsPerBranch)
					break;
				const glm::vec3 next = glm::normalize(tilt(direction, spec.bendDeg) + glm::vec3(0, spec.upwardBias, 0));
				// parallel transport keeps consecutive rings from twisting
				normal = glm::normalize(normal - glm::dot(normal, next) * next);
				direction = next;
				position += direction * step;
			}
			emitTube(rings);

			if (depth >= spec.maxDepth)
				return;
			const Ring& tip = rings.back();
			// start slightly behind the tip so the child's open base is buried in the parent
			growBranch(tip.center - tip.direction * tip.radius, tilt(tip.direction, spec.leaderAngleDeg), length * spec.leaderLengthScale, tip.radius, depth + 1);

			const uint32_t sides = uniformInt(spec.sideBranchesMin, spec.sideBranchesMax);
			const float azimuthStart = uniform(0.f, glm::two_pi<float>());
			for (uint32_t i = 0; i < sides; i++) {
				const auto attachRing = [&](float fraction) { return static_cast<uint32_t>(fraction * static_cast<float>(spec.ringsPerBranch)); };
				const Ring& at = rings[uniformInt(attachRing(spec.sideAttachStart), attachRing(spec.sideAttachEnd))];
				const float azimuth = azimuthStart + glm::two_pi<float>() * (static_cast<float>(i) + uniform(-0.2f, 0.2f)) / static_cast<float>(sides);
				const glm::vec3 axis = glm::angleAxis(azimuth, at.direction) * at.normal;
				const float angle = glm::radians(spec.sideAngleDeg * uniform(0.7f, 1.3f));
				const glm::vec3 childDirection = glm::normalize(glm::angleAxis(angle, axis) * at.direction);
				growBranch(at.center, childDirection, length * spec.sideLengthScale * uniform(0.8f, 1.1f), at.radius * spec.childRadiusScale, depth + 1);
			}
		}
	};
}

// built at the local origin with the trunk base on y = 0, place it with appendMesh's transform
inline Mesh buildTreeMesh(const TreeSpec& spec = {}) {
	Mesh mesh;
	detail::TreeBuilder builder{.spec = spec, .mesh = mesh, .rng = std::mt19937(spec.seed)};
	builder.growBranch(glm::vec3(0.f), glm::vec3(0, 1, 0), spec.trunkLength, spec.trunkRadius, 0);
	return mesh;
}
