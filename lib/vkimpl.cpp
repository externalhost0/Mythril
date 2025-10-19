//
// Created by Hayden Rivas on 10/7/25.
//
#define VOLK_IMPLEMENTATION
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...) {  \
    printf((format), __VA_ARGS__);          \
    printf("\n");           			    \
}
#include <vk_mem_alloc.h>