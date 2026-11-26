#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

// wasd to move, space and left ctrl for up and down, left shift to go faster, c toggles mouse look
class FlyCamera {
public:
	struct Options {
		// -90 yaw faces -z
		float yaw = -90.f;
		float pitch = -8.f;
		float speed = 4.f;
		float farPlane = 100.f;
	};
	FlyCamera(SDL_Window* window, const glm::vec3 position, const Options& options) :
		_window(window),
		_position(position),
		_yaw(options.yaw),
		_pitch(options.pitch),
		_speed(options.speed),
		_far(options.farPlane) {
		SDL_SetWindowRelativeMouseMode(_window, _mouseLook);
	}

	void handleEvent(const SDL_Event& e) {
		if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_C)
			SDL_SetWindowRelativeMouseMode(_window, _mouseLook = !_mouseLook);
		if (e.type == SDL_EVENT_MOUSE_MOTION && _mouseLook) {
			_yaw += e.motion.xrel * kMouseSensitivity;
			_pitch = glm::clamp(_pitch - e.motion.yrel * kMouseSensitivity, -89.f, 89.f);
		}
	}

	void update(const float deltaTime) {
		_forward = glm::normalize(glm::vec3(
			cos(glm::radians(_yaw)) * cos(glm::radians(_pitch)),
			sin(glm::radians(_pitch)),
			sin(glm::radians(_yaw)) * cos(glm::radians(_pitch))));
		const glm::vec3 right = glm::normalize(glm::cross(_forward, kUp));

		const bool* state = SDL_GetKeyboardState(nullptr);
		const float speed = (state[SDL_SCANCODE_LSHIFT] ? 3.f * _speed : _speed) * deltaTime;
		if (state[SDL_SCANCODE_W]) _position += _forward * speed;
		if (state[SDL_SCANCODE_S]) _position -= _forward * speed;
		if (state[SDL_SCANCODE_A]) _position -= right * speed;
		if (state[SDL_SCANCODE_D]) _position += right * speed;
		if (state[SDL_SCANCODE_SPACE]) _position += kUp * speed;
		if (state[SDL_SCANCODE_LCTRL]) _position -= kUp * speed;
	}

	glm::vec3 position() const { return _position; }
	bool mouseLook() const { return _mouseLook; }
	float nearPlane() const { return kNear; }
	float farPlane() const { return _far; }
	glm::mat4 view() const { return glm::lookAt(_position, _position + _forward, kUp); }
	glm::mat4 projection(const float aspectRatio, const bool reverseDepth = false) const {
		return glm::perspective(glm::radians(kFov), aspectRatio, reverseDepth ? _far : kNear, reverseDepth ? kNear : _far);
	}

private:
	static constexpr glm::vec3 kUp = {0.f, 1.f, 0.f};
	static constexpr float kMouseSensitivity = 0.3f;
	static constexpr float kFov = 80.f;
	static constexpr float kNear = 0.1f;

	SDL_Window* _window;
	glm::vec3 _position;
	glm::vec3 _forward = {0.f, 0.f, -1.f};
	float _yaw;
	float _pitch;
	float _speed;
	float _far;
	bool _mouseLook = false;
};
