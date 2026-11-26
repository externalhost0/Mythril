//
// Created by Hayden Rivas on 11/21/26.
//

#pragma once

#if defined(MYTH_ENABLED_TRACY)
#include <tracy/Tracy.hpp>

// colors
// command buffer colors
#define MYTH_PROFILER_COLOR_WAIT 0x8a89a1 // blue gray
#define MYTH_PROFILER_COLOR_ACQUIRE 0x3128de // rich blue
#define MYTH_PROFILER_COLOR_SUBMIT 0x6b28de // dark purple

#define MYTH_PROFILER_COLOR_PRESENT 0xbf1bb7 // magenta
#define MYTH_PROFILER_COLOR_BARRIER 0xbdd1ff // baby blue
#define MYTH_PROFILER_COLOR_COMMAND 0x3f449e // light saturated blue

#define MYTH_PROFILER_COLOR_UPDATE 0xd1c12e // light yellow

// function colors
#define MYTH_PROFILER_COLOR_CREATE 0x22bf4f // dark green
#define MYTH_PROFILER_COLOR_DESTROY 0xc41212 // dark red


// rendering colors
#define MYTH_PROFILER_COLOR_RENDERGRAPH 0xf0784d // light orange
#define MYTH_PROFILER_COLOR_RENDERPASS 0xff591c // orange

// macro functions
#define MYTH_PROFILER_FUNCTION_N(name) ZoneScopedN(name)
#if defined(_MSC_VER)
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__FUNCSIG__)
#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__FUNCSIG__, color)
#else
#if defined(__APPLE__)
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__PRETTY_FUNCTION__)

#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__PRETTY_FUNCTION__, color)

#else
#define MYTH_PROFILER_FUNCTION() ZoneScopedN(__PRETTY_FUNCTION__)
#define MYTH_PROFILER_FUNCTION_COLOR(color) ZoneScopedNC(__PRETTY_FUNCTION__, color)
#endif
#endif

#define MYTH_PROFILER_ZONE_COLOR(name, color) \
	{ \
		ZoneScopedNC(name, color);

#define MYTH_PROFILER_ZONE(name) \
	{ \
		ZoneScopedN(name);

#define MYTH_PROFILER_ZONE_END() }


#define MYTH_PROFILER_THREAD(name) tracy::SetThreadName(name)
#define MYTH_PROFILER_FRAME() FrameMark;

#else
#define MYTH_PROFILER_FUNCTION()
#define MYTH_PROFILER_FUNCTION_N(name)
#define MYTH_PROFILER_FUNCTION_COLOR(color)
#define MYTH_PROFILER_ZONE_COLOR(name, color) {
#define MYTH_PROFILER_ZONE(name) {
#define MYTH_PROFILER_ZONE_END() }
#define MYTH_PROFILER_THREAD(name)
#define MYTH_PROFILER_FRAME(name)
#endif // MYTH_ENABLED_TRACY
