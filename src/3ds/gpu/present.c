/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Minimal Vulkan-only framebuffer presentation using volk (no linked Vulkan
   library, no shaders): upload the guest framebuffer, convert it to RGBA8 on
   the device and show it live in an SDL3 window through a Vulkan swapchain.
   Without a display the window path is unavailable; frames then go only to
   bounded PPM snapshots in frame_dir. The 3DS framebuffer is 240x400/320
   portrait; both the window and the snapshots show it rotated 90 degrees
   clockwise, like the console's landscape top screen. */
#include "gpu/present.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <volk.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkSemaphore acquire_sem, present_sem;
    const char* frame_dir;
    unsigned dumps;
    int up, windowed;
    SDL_Window* window;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage swap_images[8];
    uint32_t swap_count;
    VkExtent2D swap_extent;
    VkImage rot_image; /* the landscape frame, scaled into the window */
    VkDeviceMemory rot_memory;
    unsigned char* rotated;
    unsigned rot_w, rot_h;
} Presenter;
static Presenter v;

static uint32_t memory_type(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(v.phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags &
                                   (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                      (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            return i;
    return UINT32_MAX;
}
static bool make_buffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(v.device, &bi, NULL, buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v.device, *buffer, &req);
    uint32_t type = memory_type(req.memoryTypeBits);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(v.device, &ai, NULL, memory) != VK_SUCCESS ||
        vkBindBufferMemory(v.device, *buffer, *memory, 0) != VK_SUCCESS)
        return false;
    return mapped ? vkMapMemory(v.device, *memory, 0, req.size, 0, mapped) == VK_SUCCESS : true;
}
static bool make_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage* image,
                       VkDeviceMemory* memory) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v.device, &ii, NULL, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v.device, *image, &req);
    uint32_t type = memory_type(req.memoryTypeBits);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    return type != UINT32_MAX && vkAllocateMemory(v.device, &ai, NULL, memory) == VK_SUCCESS &&
           vkBindImageMemory(v.device, *image, *memory, 0) == VK_SUCCESS;
}
static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next, VkAccessFlags before,
                    VkAccessFlags after) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = before,
                              .dstAccessMask = after,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}
static void swap_destroy(void) {
    if (v.rot_image) vkDestroyImage(v.device, v.rot_image, NULL);
    if (v.rot_memory) vkFreeMemory(v.device, v.rot_memory, NULL);
    v.rot_image = VK_NULL_HANDLE;
    v.rot_memory = VK_NULL_HANDLE;
    if (v.swapchain) vkDestroySwapchainKHR(v.device, v.swapchain, NULL);
    v.swapchain = VK_NULL_HANDLE;
    v.swap_count = 0;
}
static bool swap_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps) != VK_SUCCESS) return false;
    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &count, NULL) != VK_SUCCESS || !count || count > 64)
        return false;
    VkSurfaceFormatKHR* formats = calloc(count, sizeof *formats);
    if (!formats) return false;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format != VK_FORMAT_B8G8R8A8_UNORM && formats[i].format != VK_FORMAT_R8G8B8A8_UNORM) continue;
        chosen = formats[i];
        break;
    }
    free(formats);
    VkExtent2D extent = caps.currentExtent.width ? caps.currentExtent : v.swap_extent;
    if (extent.width < 2) extent = (VkExtent2D){400, 240};
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = v.surface,
                                   .minImageCount = images,
                                   .imageFormat = chosen.format,
                                   .imageColorSpace = chosen.colorSpace,
                                   .imageExtent = extent,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = caps.currentTransform,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                   .clipped = VK_TRUE};
    if (v.swapchain) {
        vkDestroySwapchainKHR(v.device, v.swapchain, NULL);
        v.swapchain = VK_NULL_HANDLE;
        v.swap_count = 0;
    }
    if (vkCreateSwapchainKHR(v.device, &ci, NULL, &v.swapchain) != VK_SUCCESS) return false;
    v.swap_extent = extent;
    uint32_t n = 8;
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &n, NULL);
    if (n > 8) n = 8;
    v.swap_count = n;
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &n, v.swap_images);
    return true;
}
bool snx_ctr_present_init(const char* frame_dir) {
    if (v.up) return true;
    v.frame_dir = frame_dir;
    if (volkInitialize() != VK_SUCCESS) return false;
    /* Live window first: a display server and Vulkan surface make this the
       primary output. Without one the same frames go to PPM snapshots. */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        v.window = SDL_CreateWindow("ScarletNX 3DS", 800, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "scarletnx-ctr",
                             .apiVersion = VK_API_VERSION_1_0};
    Uint32 extension_count = 0;
    const char* const* extensions = v.window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : NULL;
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = v.window ? extension_count : 0,
                               .ppEnabledExtensionNames = v.window ? extensions : NULL};
    if (vkCreateInstance(&ii, NULL, &v.instance) != VK_SUCCESS) {
        if (v.window) { /* retry without the window surface */
            SDL_DestroyWindow(v.window);
            v.window = NULL;
            ii.enabledExtensionCount = 0;
            ii.ppEnabledExtensionNames = NULL;
            if (vkCreateInstance(&ii, NULL, &v.instance) != VK_SUCCESS) return false;
        } else
            return false;
    }
    volkLoadInstance(v.instance);
    if (v.window && !SDL_Vulkan_CreateSurface(v.window, v.instance, NULL, &v.surface)) {
        SDL_DestroyWindow(v.window);
        v.window = NULL;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(v.instance, &count, NULL) != VK_SUCCESS || !count) return false;
    VkPhysicalDevice* devices = calloc(count, sizeof *devices);
    if (!devices) return false;
    vkEnumeratePhysicalDevices(v.instance, &count, devices);
    for (uint32_t i = 0; i < count && !v.device; ++i) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        VkQueueFamilyProperties* props = calloc(families, sizeof *props);
        if (!props) break;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, props);
        for (uint32_t f = 0; f < families; ++f) {
            VkBool32 present = VK_TRUE;
            if (v.surface && vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, v.surface, &present) != VK_SUCCESS)
                present = VK_FALSE;
            if (!(props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            /* a windowed present needs the swapchain device extension */
            const char* swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1};
            if (v.surface) {
                uint32_t extensions = 0;
                bool supports_swapchain = false;
                if (vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extensions, NULL) == VK_SUCCESS &&
                    extensions && extensions <= 256) {
                    VkExtensionProperties* list = calloc(extensions, sizeof *list);
                    if (list) {
                        vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extensions, list);
                        for (uint32_t e = 0; e < extensions; ++e)
                            if (!strcmp(list[e].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                                supports_swapchain = true;
                        free(list);
                    }
                }
                if (!supports_swapchain) continue;
                di.enabledExtensionCount = 1;
                di.ppEnabledExtensionNames = &swapchain_ext;
            }
            float priority = 1.0f;
            VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = f,
                                          .queueCount = 1,
                                          .pQueuePriorities = &priority};
            di.pQueueCreateInfos = &qi;
            if (vkCreateDevice(devices[i], &di, NULL, &v.device) != VK_SUCCESS) break;
            v.phys = devices[i];
            v.family = f;
            vkGetDeviceQueue(v.device, f, 0, &v.queue);
            break;
        }
        free(props);
    }
    free(devices);
    if (!v.device) return false;
    volkLoadDevice(v.device);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = v.family};
    if (vkCreateCommandPool(v.device, &pi, NULL, &v.pool) != VK_SUCCESS) return false;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(v.device, &si, NULL, &v.acquire_sem);
    vkCreateSemaphore(v.device, &si, NULL, &v.present_sem);
    if (v.surface) {
        VkSurfaceCapabilitiesKHR caps;
        v.swap_extent = (VkExtent2D){800, 480};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps) == VK_SUCCESS &&
            caps.currentExtent.width)
            v.swap_extent = caps.currentExtent;
        v.windowed = swap_create();
    }
    v.up = 1;
    return true;
}
bool snx_ctr_present_ready(void) { return v.up; }
bool snx_ctr_present_windowed(void) { return v.windowed; }
static void write_ppm(unsigned screen, unsigned width, unsigned height, const unsigned char* rgba) {
    if (!v.frame_dir || v.dumps >= 8) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame-%u-%02u.ppm", v.frame_dir, screen, v.dumps);
    FILE* file = fopen(path, "wb");
    if (!file) return;
    /* the console screens are landscape: rotate the portrait framebuffer 90
       degrees clockwise for the saved image */
    fprintf(file, "P6\n%u %u\n255\n", height, width);
    for (uint32_t y = 0; y < width; ++y)        /* destination rows */
        for (uint32_t x = 0; x < height; ++x) { /* destination columns */
            const unsigned char* p = rgba + ((uint64_t)(height - 1 - x) * width + y) * 4;
            const unsigned char rgb[3] = {p[0], p[1], p[2]};
            if (fwrite(rgb, 1, 3, file) != 3) break;
        }
    fclose(file);
    ++v.dumps;
}
/* Live presentation of the landscape frame to the window's swapchain. */
static bool window_show(const unsigned char* rgba, unsigned width, unsigned height) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {}
    if (!v.windowed) return false;
    unsigned landscape_w = height, landscape_h = width;
    if (!v.rotated || v.rot_w != landscape_w || v.rot_h != landscape_h) {
        free(v.rotated);
        v.rotated = malloc((size_t)landscape_w * landscape_h * 4);
        if (!v.rotated) return false;
        v.rot_w = landscape_w;
        v.rot_h = landscape_h;
        if (v.rot_image) vkDestroyImage(v.device, v.rot_image, NULL);
        if (v.rot_memory) vkFreeMemory(v.device, v.rot_memory, NULL);
        v.rot_image = VK_NULL_HANDLE;
        v.rot_memory = VK_NULL_HANDLE;
        if (!make_image(landscape_w, landscape_h, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &v.rot_image, &v.rot_memory))
            return false;
    }
    for (uint32_t y = 0; y < landscape_h; ++y)
        for (uint32_t x = 0; x < landscape_w; ++x) {
            const unsigned char* p = rgba + ((uint64_t)(height - 1 - x) * width + y) * 4;
            memcpy(v.rotated + ((size_t)y * landscape_w + x) * 4, p, 4);
        }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* mapped = NULL;
    if (!make_buffer((VkDeviceSize)landscape_w * landscape_h * 4, &staging, &staging_mem, &mapped)) return false;
    memcpy(mapped, v.rotated, (size_t)landscape_w * landscape_h * 4);
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(v.device, v.swapchain, UINT64_MAX, v.acquire_sem, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        v.windowed = swap_create();
        vkDestroyBuffer(v.device, staging, NULL);
        vkFreeMemory(v.device, staging_mem, NULL);
        return v.windowed && window_show(rgba, width, height);
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vkDestroyBuffer(v.device, staging, NULL);
        vkFreeMemory(v.device, staging_mem, NULL);
        return false;
    }
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = v.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(v.device, &ai, &cb) != VK_SUCCESS) {
        vkDestroyBuffer(v.device, staging, NULL);
        vkFreeMemory(v.device, staging_mem, NULL);
        return false;
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    barrier(cb, v.rot_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {landscape_w, landscape_h, 1}};
    vkCmdCopyBufferToImage(cb, staging, v.rot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, v.rot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImage target = v.swap_images[index];
    barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(int32_t)landscape_w, (int32_t)landscape_h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(int32_t)v.swap_extent.width, (int32_t)v.swap_extent.height, 1}}};
    vkCmdBlitImage(cb, v.rot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v.acquire_sem,
                           .pWaitDstStageMask = &wait_stage,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cb,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &v.present_sem};
    bool ok = vkQueueSubmit(v.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &v.present_sem,
                                .swapchainCount = 1,
                                .pSwapchains = &v.swapchain,
                                .pImageIndices = &index};
    VkResult shown = ok ? vkQueuePresentKHR(v.queue, &present) : VK_ERROR_INITIALIZATION_FAILED;
    vkQueueWaitIdle(v.queue);
    vkFreeCommandBuffers(v.device, v.pool, 1, &cb);
    vkDestroyBuffer(v.device, staging, NULL);
    vkFreeMemory(v.device, staging_mem, NULL);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) v.windowed = swap_create();
    return shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR;
}
bool snx_ctr_present_frame(unsigned screen, unsigned width, unsigned height, unsigned format, unsigned pitch,
                           const unsigned char* pixels, uint64_t* hash) {
    if (!v.up || !pixels || !width || !height || width > 1024 || height > 1024 || (format != 0 && format != 2))
        return false;
    uint32_t bytes = format == 0 ? 4 : 2;
    if (pitch < width * bytes) return false;
    VkFormat source = format == 0 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R5G6B5_UNORM_PACK16;
    VkDeviceSize raw_size = (VkDeviceSize)pitch * height;
    VkBuffer staging = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE, readback_mem = VK_NULL_HANDLE;
    VkImage src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
    void *raw = NULL, *out = NULL;
    bool ok = make_buffer(raw_size, &staging, &staging_mem, &raw) &&
              make_buffer((VkDeviceSize)width * height * 4, &readback, &readback_mem, &out) &&
              make_image(width, height, source, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &src,
                         &src_mem) &&
              make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &dst, &dst_mem);
    if (ok) {
        memcpy(raw, pixels, (size_t)raw_size);
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = v.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VkCommandBuffer cb = VK_NULL_HANDLE;
        ok = vkAllocateCommandBuffers(v.device, &ai, &cb) == VK_SUCCESS;
        if (ok) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkBeginCommandBuffer(cb, &bi);
            barrier(cb, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        .imageExtent = {width, height, 1}};
            vkCmdCopyBufferToImage(cb, staging, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}}};
            vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_NEAREST);
            barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            ok = vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(v.queue) == VK_SUCCESS;
            vkFreeCommandBuffers(v.device, v.pool, 1, &cb);
        }
    }
    if (ok) {
        uint64_t hash_value = 1469598103934665603ull;
        const unsigned char* rgba = (const unsigned char*)out;
        for (uint64_t i = 0; i < (uint64_t)width * height * 4; ++i)
            hash_value = (hash_value ^ rgba[i]) * 1099511628211ull;
        if (hash) *hash = hash_value;
        write_ppm(screen, width, height, rgba);
        if (screen == 0) window_show(rgba, width, height);
    }
    if (readback) vkDestroyBuffer(v.device, readback, NULL);
    if (readback_mem) vkFreeMemory(v.device, readback_mem, NULL);
    if (staging) vkDestroyBuffer(v.device, staging, NULL);
    if (staging_mem) vkFreeMemory(v.device, staging_mem, NULL);
    if (src) vkDestroyImage(v.device, src, NULL);
    if (src_mem) vkFreeMemory(v.device, src_mem, NULL);
    if (dst) vkDestroyImage(v.device, dst, NULL);
    if (dst_mem) vkFreeMemory(v.device, dst_mem, NULL);
    return ok;
}
void snx_ctr_present_free(void) {
    if (!v.up) return;
    vkDeviceWaitIdle(v.device);
    swap_destroy();
    if (v.acquire_sem) vkDestroySemaphore(v.device, v.acquire_sem, NULL);
    if (v.present_sem) vkDestroySemaphore(v.device, v.present_sem, NULL);
    free(v.rotated);
    vkDestroyCommandPool(v.device, v.pool, NULL);
    if (v.surface) vkDestroySurfaceKHR(v.instance, v.surface, NULL);
    vkDestroyDevice(v.device, NULL);
    vkDestroyInstance(v.instance, NULL);
    if (v.window) {
        SDL_DestroyWindow(v.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    memset(&v, 0, sizeof v);
}
