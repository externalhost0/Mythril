//
// Created by Hayden Rivas on 12/5/25.
//

#pragma once

#include <glm/glm.hpp>

namespace GPU {
	struct GeneralVertex {
		glm::vec3 position;
		float uv_x;
		glm::vec3 normal;
		float uv_y;
		glm::vec4 tangent;
	};

	struct CameraData {
		glm::mat4 proj;
		glm::mat4 invProj;
		glm::mat4 view;
		glm::vec3 position;
		glm::vec2 uvToViewA;
		glm::vec2 uvToViewB;
		float far;
		float near;
	};

	struct EnvironmentLight {
		glm::vec3 color;
		float intensity;
	};
	struct DirectionalLight {
		glm::vec3 color;
		float intensity;
		glm::vec3 direction;
	};
	struct PointLight {
		glm::vec3 color;
		float intensity;
		glm::vec3 position;
		float range;
	};
	struct LightingData {
		PointLight pointLights[4];
		DirectionalLight directionalLights;
		EnvironmentLight environmentLight;
	};

	struct FrameData {
		CameraData camera;
		LightingData lighting;
		float time;
		float deltaTime;
		int one;
	};

	struct GeometryPushConstants {
		glm::mat4 model;
		VkDeviceAddress vba;
		glm::vec4 tintColor;
		uint64_t baseColorTexture;
		uint64_t normalTexture;
		uint64_t roughnessMetallicTexture;
		uint64_t samplerState;
		uint64_t shadowTexture;
        uint64_t shadowSampler;
		glm::mat4 lightSpaceMatrix;
        float depthBiasConstant;
        float depthBiasSlope;
	};

}