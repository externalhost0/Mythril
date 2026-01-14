//
// Created by Hayden Rivas on 1/14/26.
//

#pragma once

#include <glm/glm.hpp>

namespace Helpers {
	static glm::vec3 RGBtoHSV(glm::vec3 rgb) {
		float r = rgb.r, g = rgb.g, b = rgb.b;
		float maxC = glm::max(glm::max(r, g), b);
		float minC = glm::min(glm::min(r, g), b);
		float delta = maxC - minC;

		glm::vec3 hsv;
		hsv.z = maxC; // Value

		if (maxC == 0.0f) {
			hsv.y = 0.0f; // Saturation
			hsv.x = 0.0f; // Hue
			return hsv;
		}

		hsv.y = delta / maxC; // Saturation

		if (delta == 0.0f) {
			hsv.x = 0.0f; // Hue
		} else {
			if (r == maxC) {
				hsv.x = (g - b) / delta;
			} else if (g == maxC) {
				hsv.x = 2.0f + (b - r) / delta;
			} else {
				hsv.x = 4.0f + (r - g) / delta;
			}
			hsv.x *= 60.0f;
			if (hsv.x < 0.0f) hsv.x += 360.0f;
		}

		return hsv;
	}

	static glm::vec3 HSVtoRGB(glm::vec3 hsv) {
		float h = hsv.x, s = hsv.y, v = hsv.z;

		if (s == 0.0f) {
			return glm::vec3(v, v, v);
		}

		h = fmod(h, 360.0f) / 60.0f;
		int i = (int)floor(h);
		float f = h - i;
		float p = v * (1.0f - s);
		float q = v * (1.0f - s * f);
		float t = v * (1.0f - s * (1.0f - f));

		switch(i) {
			case 0: return glm::vec3(v, t, p);
			case 1: return glm::vec3(q, v, p);
			case 2: return glm::vec3(p, v, t);
			case 3: return glm::vec3(p, q, v);
			case 4: return glm::vec3(t, p, v);
			default: return glm::vec3(v, p, q);
		}
	}
}