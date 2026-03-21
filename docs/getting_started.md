# Getting Started
`Mythril` makes it easy to quickly create and iterate over a new Vulkan application, only requiring you to give it a surface, for a specific chapter/feature check out one of the below sections.

- [Getting Started](#getting-started)
- [Background!](#background)
- [Creation](#creation)
- [Objects](#objects)
    - [Buffers](#buffers)
    - [Images](#images)
    - [Samplers](#samplers)
    - [Shaders](#shaders)
    - [Pipelines](#pipelines)
- [FrameGraph Usage](#framegraph)
- [CommandBuffer Usage](#commandbuffer)
- [Cleanup](#cleanup)


# Background!

`Mythril` is intended to be used as the primary way you are interacting with Vulkan and should not be mixed alongside other frameworks.

You are expected to manage the windowing system yourself, however `Mythril` is built to allow ANY vulkan

The majority of operations you will be performing are the creation of our Vulkan objects and setting up your framegraph, and basically everything your application will do is inside the framegraph anyway.

# Creation

The `mythril::CTX` type is how you will primarily build out your app, it can only be instanced via the `mythril::CTXBuilder` as many important options need to be configured before we can build the Vulkan context. You should only ever create one `mythril::CTX`!

If you want a headless Vulkan context:
```cpp
auto ctx = mythril::CTXBuilder{}
.build();
```
However you likely want a Vulkan context that can present images to the screen, which means you **MUST** call `set_window_surface()`, it requires both the code necessary to create and destroy a `VkSurfaceKHR`, which mean

```cpp
auto ctx = mythril::CTXBuilder{}
.set_window_surface(
[sdlWindow](VkInstance instance) {
	VkSurfaceKHR surface;
	SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
	return surface;
},
[](VkInstance instance, VkSurfaceKHR surface_khr) {
	SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
})
.with_default_swapchain({
	.width = width,
	.height = height,
})
.build();
```
You may notice that we also call `with_default_swapchain` with whatever width and height our window is, it is not necessary to call this function from `mythril::CTXBuilder` however it does mean less boilerplate. The above code will do the same thing if you defer swapchain creation as such:

```cpp
auto ctx = mythril::CTXBuilder{}
.set_window_surface(
[sdlWindow](VkInstance instance) {
	VkSurfaceKHR surface;
	SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
	return surface;
},
[](VkInstance instance, VkSurfaceKHR surface_khr) {
	SDL_Vulkan_DestroySurface(instance, surface_khr, nullptr);
})
.build();
ctx->createSwapchain({
	.width = width,
	.height = height
});
```

Once you have your `mythril::CTX` instance you can do anything!

# Objects
[wip]

[//]: # (## Buffers)

[//]: # ()
[//]: # (## Images)

[//]: # ()
[//]: # (## Samplers)

[//]: # ()
[//]: # (## Shaders)

[//]: # ()
[//]: # (## Pipelines)

# Cleanup

There is no manual cleanup necessary, as `Mythril`'s Vulkan objects will call the necessary destruction logic when their destructor is called. The same goes for `mythril::CTX` which will perform cleanup for all previously created objects if their destructors have not already been called.

What you do have to do is keep the windowing system/context alive until AFTER `mythril::CTX` is destroyed, as destroying your window, ie `SDL_WINDOW*` too early will result in validation errors. This is why all the samples scope `mythril::CTX` but not the created window.