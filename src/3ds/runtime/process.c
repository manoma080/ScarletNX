/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_process.h"
#include <stdlib.h>

struct SnxCtrProcess {
    SnxCtrMemory* memory;
    SnxCtrAddressSpace* space;
    SnxCtrCpu cpu;
    uint32_t physical_next, physical_end;
};
static bool map(SnxCtrProcess* p, uint32_t va, uint32_t size, unsigned permissions) {
    if (!size) return true;
    if ((uint64_t)p->physical_next + size > p->physical_end ||
        snx_ctr_map(p->space, va, p->physical_next, size, permissions) != SNX_CTR_MEM_OK) return false;
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
    SnxCtrProcess* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->physical_next = 0x20000000;
    p->physical_end = p->physical_next + profile->fcram_size;
    p->memory = snx_ctr_memory_new(model);
    if (!p->memory || !(p->space = snx_ctr_space_new(p->memory))) goto fail;
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const SnxCtrSegment* s = &image->segment[i];
        uint64_t bytes = (uint64_t)s->pages * 4096;
        uint64_t size = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~UINT64_C(4095) : 0);
        if (size > UINT32_MAX || bytes > image->code_size - offset ||
            !map(p, s->address, (uint32_t)size, SNX_CTR_READ | SNX_CTR_WRITE)) goto fail;
        if (bytes && snx_ctr_write(p->space, s->address, image->code + offset, (size_t)bytes) != SNX_CTR_MEM_OK) goto fail;
        unsigned permissions = i == 0 ? SNX_CTR_READ | SNX_CTR_EXEC : i == 1 ? SNX_CTR_READ : SNX_CTR_READ | SNX_CTR_WRITE;
        if (size && snx_ctr_protect(p->space, s->address, (uint32_t)size, permissions) != SNX_CTR_MEM_OK) goto fail;
        offset += (size_t)bytes;
    }
    uint32_t stack = (image->stack_size + 4095) & ~4095u;
    if (offset != image->code_size || !map(p, SNX_CTR_STACK_TOP - stack, stack, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !map(p, SNX_CTR_INITIAL_TLS, 4096, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !snx_ctr_cpu_init(&p->cpu, p->space, image->segment[0].address, SNX_CTR_STACK_TOP, false)) goto fail;
    p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
    return p;
fail:
    snx_ctr_process_free(p);
    return NULL;
}
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* p) { return p ? &p->cpu.state.cpu : NULL; }
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* p) { return p ? p->space : NULL; }
