//
// Created by Hayden Rivas on 10/22/25.
//

#include "Window.h"
#include "mythril/CTXBuilder.h"
#include "Logger.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>

namespace mythril {
	void Window::create(const WindowSpec& spec) {
		_isWindowManuallyResizable = spec.resizeable;

		constexpr SDL_WindowFlags required_window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
		SDL_Window* sdlWindow = SDL_CreateWindow(spec.title.c_str(), spec.width, spec.height, required_window_flags | (spec.resizeable ? SDL_WINDOW_RESIZABLE : 0));
		ASSERT_MSG(sdlWindow != nullptr, "SDL window could not be created! Error: {}", SDL_GetError());
		_sdlWindow = sdlWindow;
		 _windowMode = spec.mode;
	}
	void Window::destroy() {
		ASSERT_MSG(_sdlWindow, "SDL window has not been created and therefore cant be destroyed!");
		SDL_DestroyWindow(_sdlWindow);
	}

	VkExtent2D Window::getWindowSize() const {
		int w, h;
		SDL_GetWindowSize(_sdlWindow, &w, &h);
		return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
	}
	VkExtent2D Window::getFramebufferSize() const {
		int w, h;
		SDL_GetWindowSizeInPixels(_sdlWindow, &w, &h);
		return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
	}
	float Window::getContentScale() const {
		float scale = SDL_GetWindowDisplayScale(_sdlWindow);
		return scale;
	}

	WindowMode Window::getWindowMode() const {
		return _windowMode;
	}

	void Window::setWindowSize(VkExtent2D newExtent) {
		ASSERT_MSG(_sdlWindow, "Window must be created before it can be operated on!");
		SDL_SetWindowSize(_sdlWindow, static_cast<int>(newExtent.width), static_cast<int>(newExtent.height));
	}
	void Window::setWindowMode(WindowMode newMode) {
		ASSERT_MSG(newMode != WindowMode::Headless, "You cannot set display mode to Headless during execution.");
		switch (newMode) {
			case WindowMode::Windowed: {
				if (!SDL_SetWindowFullscreen(_sdlWindow, false)) {
					LOG_SYSTEM(LogType::Error, "Failed to set window mode as Windowed. SDL Error: {}", SDL_GetError());
				}
				break;
			}
			case WindowMode::Fullscreen: {
				if (!SDL_SetWindowFullscreen(_sdlWindow, true)) {
					LOG_SYSTEM(LogType::Error, "Failed to set window mode as Borderless Fullscreen. SDL Error: {}", SDL_GetError());
				}
				break;
			}
		}
		_windowMode = newMode;
	}

	void Window::setMouseMode(bool relativeEnabled) {
		SDL_SetWindowRelativeMouseMode(this->_sdlWindow, relativeEnabled);
	}
}