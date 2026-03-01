//
// Created by Hayden Rivas on 2/28/26.
//

#pragma once
#include <cassert>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

inline SDL_Window* BuildSDLWindow(bool isResizable) {
    const bool sdl_initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    assert(sdl_initialized && "SDL could not be initialized!");
    constexpr SDL_WindowFlags required_window_flags = SDL_WINDOW_VULKAN;
    SDL_Window* sdlWindow = SDL_CreateWindow("My Custom Window", 1280, 720, required_window_flags | SDL_WINDOW_HIGH_PIXEL_DENSITY | (isResizable ? SDL_WINDOW_RESIZABLE : 0));
    assert(sdlWindow != nullptr && "SDL window could not be created!");
    return sdlWindow;
}
inline void DestroySDLWindow(SDL_Window* sdlWindow) {
    SDL_DestroyWindow(sdlWindow);
	SDL_Quit();
}
struct Dims {
	uint32_t width;
	uint32_t height;
};
inline Dims GetSDLWindowFramebufferSize(SDL_Window* sdlWindow) {
	int w, h;
	SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
	return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
}