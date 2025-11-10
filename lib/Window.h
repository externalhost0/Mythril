//
// Created by Hayden Rivas on 10/22/25.
//

#pragma once


#include <volk.h>
#include <SDL3/SDL_video.h>

namespace mythril {
	enum class WindowMode;
	struct WindowSpec;

	class Window {
	public:
		Window() = default;

		void create(const WindowSpec& spec);
		void destroy();

		void setWindowMode(WindowMode mode);
		void setWindowSize(VkExtent2D newExtent);

		VkExtent2D getWindowSize() const;
		VkExtent2D getFramebufferSize() const;
		WindowMode getWindowMode() const;
		float getContentScale() const;
	private:
		inline SDL_Window* _getSDLwindow() { return _sdlWindow; }
	private:
		SDL_Window* _sdlWindow = nullptr;
		WindowMode _windowMode{};

		// this means the ability to drag the window with a users cursor to perform resizing
		bool _isWindowManuallyResizable = false;

		friend class CTXBuilder;
	};
}