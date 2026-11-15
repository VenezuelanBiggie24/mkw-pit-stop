// Vulkan Memory Allocator implementation (AMD, MIT; from the MSYS2 package vulkan-memory-allocator, not copied).
// VMA is C++ only, so this is the one C++ file of the runtime; the rest uses the C interface of vk_mem_alloc.h.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vulkan/vulkan.h>
#include "vk_destroy_count.h"   // VMA's buffer destructions count for MKW_DRAW_CACHE
// Use the counted global dispatch above even when those globals come from
// Volk. Its optional VMA table importer cannot use our counted member name.
#ifdef VOLK_HEADER_VERSION
#undef VOLK_HEADER_VERSION
#endif
#include "vk_mem_alloc.h"
