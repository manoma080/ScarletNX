/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "internal.h"
#include <stdlib.h>
#include <string.h>
static bool map(SnxCtrProcess* p, uint32_t va, uint32_t size, unsigned permissions) {
    if (!size) return true;
    if ((uint64_t)p->physical_next + size > p->physical_end ||
        snx_ctr_map(p->space, va, p->physical_next, size, permissions) != SNX_CTR_MEM_OK)
        return false;
    memset(p->physical_owner + (p->physical_next - CTR_PHYS_BASE) / 4096, CTR_STATIC, size / 4096);
    p->physical_next += size;
    return true;
}
void snx_ctr_process_free(SnxCtrProcess* p) {
    if (!p) return;
    snx_ctr_space_free(p->space);
    snx_ctr_memory_free(p->memory);
    free(p);
}
SnxCtrProcess* snx_ctr_process_new(const SnxCtrImage* image, SnxCtrModel model) {
    const SnxCtrProfile* profile = snx_ctr_profile(model);
    if (!image || !image->code || !profile || !image->stack_size || image->stack_size > 0x800000) return NULL;
    uint32_t app_limit = model == SNX_CTR_OLD ? 0x4000000 : 0x7c00000;
    SnxCtrProcess* p = calloc(1, sizeof *p + app_limit / 4096);
    if (!p) return NULL;
    p->app_limit = app_limit;
    p->physical_next = CTR_PHYS_BASE;
    p->physical_end = p->physical_next + p->app_limit;
    p->objects[1] = (CtrObject){.type = CTR_OBJ_PROCESS, .refs = 1};
    p->objects[2] = (CtrObject){.type = CTR_OBJ_THREAD, .refs = 1};
    p->current_thread = 2;
    p->objects[2].data.thread.priority = 0x30;
    p->memory = snx_ctr_memory_new(model);
    if (!p->memory || !(p->space = snx_ctr_space_new(p->memory))) goto fail;
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const SnxCtrSegment* s = &image->segment[i];
        uint64_t bytes = (uint64_t)s->pages * 4096;
        uint64_t size = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~UINT64_C(4095) : 0);
        if (size > UINT32_MAX || bytes > image->code_size - offset ||
            !map(p, s->address, (uint32_t)size, SNX_CTR_READ | SNX_CTR_WRITE))
            goto fail;
        if (bytes && snx_ctr_write(p->space, s->address, image->code + offset, (size_t)bytes) != SNX_CTR_MEM_OK)
            goto fail;
        unsigned permissions = i == 0   ? SNX_CTR_READ | SNX_CTR_EXEC
                               : i == 1 ? SNX_CTR_READ
                                        : SNX_CTR_READ | SNX_CTR_WRITE;
        if (size && snx_ctr_protect(p->space, s->address, (uint32_t)size, permissions) != SNX_CTR_MEM_OK) goto fail;
        offset += (size_t)bytes;
    }
    uint32_t stack = (image->stack_size + 4095) & ~4095u;
    if (offset != image->code_size || !map(p, SNX_CTR_STACK_TOP - stack, stack, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !map(p, SNX_CTR_INITIAL_TLS, 4096, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !snx_ctr_cpu_init(&p->cpu, p->space, image->segment[0].address, SNX_CTR_STACK_TOP, false))
        goto fail;
    p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
    p->objects[2].data.thread.context = p->cpu.state.cpu;
    /* Configuration-page layout and kernel personality facts from Citra/Azahar
       config_mem.{h,cpp}, memory.cpp (GPL-2.0-or-later, revision in docs/3DS.md).
       No firmware is loaded. User mappings are read-only. */
    /* Board AXI WRAM, not application FCRAM: do not charge this kernel page
       to the application's heap quota or alias it to application allocations. */
    if (snx_ctr_map(p->space, CTR_CONFIG, CTR_CONFIG, 4096, SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK) goto fail;
    unsigned char config[4096] = {0};
    config[2] = config[0x62] = 0x3a;
    config[3] = config[0x63] = 2;
    config[0x14] = 1;
    config[0x16] = 1;
    config[0x10] = config[0x64] = 2;
    config[0x18] = config[0x68] = 0x50;
    config[0x19] = config[0x69] = 0xf4;
    uint32_t sizes[] = {p->app_limit, model == SNX_CTR_OLD ? 0x2c00000u : 0x6400000u,
                        model == SNX_CTR_OLD ? 0x1400000u : 0x2000000u};
    config[0x30] = model == SNX_CTR_OLD ? 0 : 6;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) config[0x40 + i * 4 + j] = (unsigned char)(sizes[i] >> (j * 8));
    if (snx_ctr_write_ram(p->space, CTR_CONFIG, config, sizeof config) != SNX_CTR_MEM_OK ||
        snx_ctr_protect(p->space, CTR_CONFIG, 4096, SNX_CTR_READ) != SNX_CTR_MEM_OK)
        goto fail;
    return p;
fail:
    snx_ctr_process_free(p);
    return NULL;
}
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* p) { return p ? &p->cpu.state.cpu : NULL; }
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* p) { return p ? p->space : NULL; }
