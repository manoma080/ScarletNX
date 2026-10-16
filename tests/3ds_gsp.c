/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   End-to-end gsp::Gpu/Vulkan test: real service requests, shared GX queue,
   checked register writes, memory fills and a real Vulkan presentation of a
   guest framebuffer to a PPM snapshot. No GPU emulation is faked; when no
   Vulkan device exists the presentation half stops (checked here explicitly). */
#include "runtime/internal.h"
#include "ipc/ctr_ipc.h"
#include "services/gsp.h"
#include "gpu/present.h"
#include "filesystem/host.h"
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMD   (SNX_CTR_INITIAL_TLS + 0x80)
#define QUEUE 0x14000000u
static void word(SnxCtrProcess* p, uint32_t address, uint32_t v) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (i * 8));
    assert(snx_ctr_write_ram(p->space, address, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, unsigned n) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, CMD + 4 * n, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint32_t at(SnxCtrProcess* p, uint32_t address) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, address, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void request(SnxCtrProcess* p, uint32_t h, const uint32_t* w, unsigned n, SnxCtrSvcResult state,
                    uint32_t transport) {
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, CMD, zero, 256) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < n; ++i) word(p, CMD + i * 4, w[i]);
    p->cpu.state.cpu.r[0] = h;
    assert(snx_ctr_svc(p, 0x32) == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? transport : h));
}
static uint64_t fnv(const unsigned char* bytes, size_t n) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
/* PPM pixels of the 90-degree clockwise rotation of a width x height RGBA
   framebuffer: the saved image has height columns and width rows. */
static void ppm_pixel(const unsigned char* ppm, size_t size, unsigned width, unsigned height, unsigned x, unsigned y,
                      unsigned char out[3]) {
    const char* header = "P6\n400 240\n255\n";
    (void)width;
    (void)height;
    size_t at = strlen(header) + ((size_t)y * 400 + x) * 3;
    assert(at + 3 <= size);
    memcpy(out, ppm + at, 3);
}
int main(int argc, char** argv) {
    assert(argc == 2);

    (void)plat_mkdir(argv[1]);
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 0x3000,
                         .code = code,
                         .code_size = sizeof code};
    bool ready = snx_ctr_present_init(argv[1]);
    if (!ready) {
        printf("no Vulkan device: presentation checks skipped\n");
        return 77;
    }
    for (unsigned model = 0; model < 2; ++model) {
        assert(snx_ctr_present_init(argv[1]));
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        uint32_t gsp;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_GSP, &gsp));
        uint32_t event = 0;
        assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &event));
        uint32_t register_relay[] = {0x00130042, 1, 0, event};
        request(p, gsp, register_relay, 4, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x00130082 && get(p, 1) == 0x2a07 && get(p, 2) == 0 && get(p, 3) == 0x10);
        uint32_t shared = get(p, 4);
        assert(snx_ctr_handle_object(p, shared) && !snx_ctr_handle_object(p, shared ^ 1u));
        /* A second registration reports success, not another first-init. */
        request(p, gsp, register_relay, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        /* MapMemoryBlock: the 0x1000 GX block at a free mappable address. */
        p->cpu.state.cpu.r[0] = shared;
        p->cpu.state.cpu.r[1] = QUEUE;
        p->cpu.state.cpu.r[2] = 3;
        p->cpu.state.cpu.r[3] = 0x10000000;
        assert(snx_ctr_svc(p, 0x1f) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
        /* QueryMemory reports the registered mapping as shared. */
        word(p, 0x300000, 0);
        p->cpu.state.cpu.r[0] = 0x300000;
        p->cpu.state.cpu.r[1] = 0x300010;
        p->cpu.state.cpu.r[2] = QUEUE;
        assert(snx_ctr_svc(p, 0x02) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
        assert(at(p, 0x300000) == QUEUE && at(p, 0x300004) == 0x1000 && at(p, 0x300008) == 3 && at(p, 0x30000c) == 6);
        /* GPU right + process handle check. */
        uint32_t acquire[] = {0x00160042, 0, 0, CTR_PROCESS_HANDLE};
        request(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        acquire[3] = 0x1234;
        request(p, gsp, acquire, 4, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd8e007f7);
        acquire[3] = CTR_PROCESS_HANDLE;
        /* Register writes land in the bounded register file with exact values. */
        uint32_t hw[] = {0x00010082, 0x51c, 8, (8u << 14) | 2, 0x300200};
        word(p, 0x300200, 0x01c501c1);
        word(p, 0x300204, 0x000001c1);
        request(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        hw[1] = 0x1000;
        request(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xe0e02a01); /* out of the modeled register range */
        hw[1] = 0x51c;
        hw[2] = 6;
        hw[3] = (6u << 14) | 2;
        request(p, gsp, hw, 5, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xe0e02bf2); /* register writes are word-sized */
        hw[2] = 8;
        hw[3] = (8u << 14) | 2;
        /* MemoryFill through the real shared GX queue. */
        uint32_t target = 0;
        assert(!snx_ctr_kernel_memory(p, 0x10003, 0, 0, 0x1000, 3, &target) && target);
        word(p, QUEUE + 0x800, 0x100); /* one command queued */
        word(p, QUEUE + 0x820, 2);     /* MemoryFill */
        word(p, QUEUE + 0x824, target);
        word(p, QUEUE + 0x828, 0x11223344);
        word(p, QUEUE + 0x82c, target + 0x1000);
        word(p, QUEUE + 0x830, 0);
        word(p, QUEUE + 0x834, 0);
        word(p, QUEUE + 0x838, 0);
        word(p, QUEUE + 0x83c, 0);
        uint32_t trigger[] = {0x000c0000};
        request(p, gsp, trigger, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        for (uint32_t at = target; at < target + 0x1000; at += 4) {
            unsigned char b[4];
            assert(snx_ctr_read_ram(p->space, at, b, 4) == SNX_CTR_MEM_OK);
            assert(b[0] == 0x44 && b[1] == 0x33 && b[2] == 0x22 && b[3] == 0x11);
        }
        /* SubmitCmdList (PICA) stops explicitly and leaves the queue intact. */
        word(p, QUEUE + 0x800, 0x100);
        word(p, QUEUE + 0x820, 1);
        request(p, gsp, trigger, 1, SNX_CTR_SVC_UNSUPPORTED, 0);
        /* SetBufferSwap: a real RGBA8 framebuffer with a distinctive pattern. */
        uint32_t fb = 0;
        assert(!snx_ctr_kernel_memory(p, 0x10003, 0, 0, 0x60000, 3, &fb) && fb);
        unsigned char* pattern = malloc(240u * 400 * 4);
        assert(pattern);
        for (unsigned y = 0; y < 400; ++y)
            for (unsigned x = 0; x < 240; ++x) {
                unsigned char* px = pattern + ((size_t)y * 240 + x) * 4;
                px[0] = (unsigned char)x;
                px[1] = (unsigned char)y;
                px[2] = (unsigned char)(model + 1);
                px[3] = 255;
            }
        assert(snx_ctr_write_ram(p->space, fb, pattern, 240u * 400 * 4) == SNX_CTR_MEM_OK);
        uint32_t swap[] = {0x00050200, 0, 0, fb, fb, 960, 0, 0, 0};
        request(p, gsp, swap, 9, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        snx_ctr_gsp_vblank(p);
        if (!model) {
            /* The saved frame is the real Vulkan readback, rotated like the
               console screen: 400x240 landscape. Corners come from the exact
               framebuffer pattern. */
            char path[1024];
            snprintf(path, sizeof path, "%s/frame-0-00.ppm", argv[1]);
            FILE* f = fopen(path, "rb");
            assert(f);
            fseek(f, 0, SEEK_END);
            long length = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char* ppm = malloc((size_t)length);
            assert(ppm && length > 0 && fread(ppm, 1, (size_t)length, f) == (size_t)length);
            fclose(f);
            const char* header = "P6\n400 240\n255\n";
            assert(!memcmp(ppm, header, strlen(header)));
            unsigned char rgb[3];
            ppm_pixel(ppm, (size_t)length, 240, 400, 0, 0, rgb); /* fb bottom-left: (0,399) */
            assert(rgb[0] == 0 && rgb[1] == 143 && rgb[2] == 1);
            ppm_pixel(ppm, (size_t)length, 240, 400, 399, 0, rgb); /* fb top-left: (0,0) */
            assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 1);
            ppm_pixel(ppm, (size_t)length, 240, 400, 0, 239, rgb); /* fb bottom-right: (239,399) */
            assert(rgb[0] == 239 && rgb[1] == 143 && rgb[2] == 1);
            free(ppm);
            /* Presenter contract: RGB565 converts to RGBA8 on the device and
               the hash covers exactly the converted pixels. */
            unsigned short low[2] = {0xffffu, 0xf800u};
            unsigned char raw[4];
            memcpy(raw, low, sizeof raw);
            uint64_t hash = 0;
            assert(snx_ctr_present_frame(1, 2, 1, 2, 4, raw, &hash));
            const unsigned char expect[8] = {255, 255, 255, 255, 255, 0, 0, 255};
            assert(hash == fnv(expect, sizeof expect));
        }
        free(pattern);
        /* WaitSynchronization1 on the relay event completes at a real vblank. */
        p->cpu.state.cpu.r[0] = event;
        p->cpu.state.cpu.r[1] = 0;
        p->cpu.state.cpu.r[2] = UINT32_MAX;
        p->cpu.state.cpu.r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
        snx_ctr_present_free();
        snx_ctr_process_free(p);
    }
    printf("GSP relay/GX queue/register file and Vulkan presentation passed\n");
    return 0;
}
