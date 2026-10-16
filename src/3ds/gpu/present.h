/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PRESENT_H
#define SNX_CTR_PRESENT_H
#include <stdbool.h>
#include <stdint.h>
/* Minimal Vulkan-only guest-framebuffer presentation. There is no CPU or
   alternate renderer: when no Vulkan loader/device exists, init fails and
   presentation stops explicitly. frame_dir receives bounded PPM snapshots
   (the 3DS framebuffer is 240x400/320 portrait, shown rotated 90 degrees
   clockwise, like the console's landscape screens). */
bool snx_ctr_present_init(const char* frame_dir);
bool snx_ctr_present_ready(void);
/* Live window output is active (SDL3 Vulkan window + swapchain). When false,
   frames still render but only PPM snapshots record them. */
bool snx_ctr_present_windowed(void);
/* Raw guest pixels, pitch bytes per row. Format 0 = RGBA8, 2 = RGB565
   (little-endian 16-bit, R in bits 11..15, G 5..10, B 0..4). The Vulkan
   device performs the format conversion to RGBA8. */
bool snx_ctr_present_frame(unsigned screen, unsigned width, unsigned height, unsigned format, unsigned pitch,
                           const unsigned char* pixels, uint64_t* hash);
void snx_ctr_present_free(void);
#endif
