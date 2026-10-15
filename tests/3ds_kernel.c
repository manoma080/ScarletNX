/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "aot/ctr_module.h"
#include "kernel/ctr_kernel.h"
#include "runtime/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define INVALID_HANDLE 0xd8e007f7u
#define TIMEOUT        0x09401bfeu
static uint32_t word(SnxCtrProcess* p, uint32_t va) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void put(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (8 * i));
    assert(snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t used(SnxCtrProcess* p) {
    p->cpu.state.cpu.r[1] = 0;
    p->cpu.state.cpu.r[2] = 1;
    assert(snx_ctr_svc(p, 0x2a) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!p->cpu.state.cpu.r[2]);
    return p->cpu.state.cpu.r[1];
}
static uint32_t memory(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t size, uint32_t permissions,
                       uint32_t expected) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = op;
    r[1] = va;
    r[2] = 0;
    r[3] = size;
    r[4] = permissions;
    assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN && r[0] == expected);
    return r[1];
}
static uint32_t create(SnxCtrProcess* p) {
    assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return p->cpu.state.cpu.r[1];
}
static void close_handle(SnxCtrProcess* p, uint32_t handle, uint32_t expected) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_RETURN && p->cpu.state.cpu.r[0] == expected);
}
static void arbitrate(SnxCtrProcess* p, uint32_t handle, uint32_t va, uint32_t type, uint32_t value, uint64_t timeout,
                      SnxCtrSvcResult state, uint32_t result) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = handle;
    r[1] = va;
    r[2] = type;
    r[3] = value;
    r[4] = (uint32_t)timeout;
    r[5] = (uint32_t)(timeout >> 32);
    assert(snx_ctr_svc(p, 0x22) == state);
    if (state == SNX_CTR_SVC_RETURN) assert(r[0] == result);
}
static unsigned object_count(SnxCtrProcess* p) {
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) count += !!p->objects[i].type;
    return count;
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        uint32_t* r = p->cpu.state.cpu.r;
        assert(!snx_ctr_kernel_blocked(p));
        unsigned permissions;
        assert(snx_ctr_query(p->space, CTR_CONFIG, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 1);
        assert(word(p, CTR_CONFIG) == 0x023a0000);
        assert(word(p, CTR_CONFIG + 0x40) == p->app_limit);
        assert((uint64_t)word(p, CTR_CONFIG + 0x40) + word(p, CTR_CONFIG + 0x44) + word(p, CTR_CONFIG + 0x48) ==
               snx_ctr_profile((SnxCtrModel)model)->fcram_size);
        assert(snx_ctr_write_ram(p->space, CTR_CONFIG, "x", 1) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_fetch(p->space, CTR_CONFIG, code, 4) == SNX_CTR_MEM_PERMISSION);
        uint32_t baseline = used(p);
        assert(baseline == 0x9000);
        uint32_t va = memory(p, 3, CTR_HEAP, 8192, 3, 0);
        assert(va == CTR_HEAP && used(p) == baseline + 8192);
        assert(!word(p, va) && !word(p, va + 4096));
        put(p, va, 0xabcdef01);
        assert(memory(p, 6, va, 4096, 1, 0) == va);
        assert(snx_ctr_write_ram(p->space, va, "x", 1) == SNX_CTR_MEM_PERMISSION);
        memory(p, 6, va, 4096, 3, 0);
        uint64_t epoch = snx_ctr_memory_epoch(p->space);
        memory(p, 1, va + 4096, 8192, 0, 0xe0a01bf5u);
        assert(snx_ctr_memory_epoch(p->space) == epoch && used(p) == baseline + 8192 && word(p, va) == 0xabcdef01);
        memory(p, 1, va, 4096, 0, 0);
        assert(used(p) == baseline + 4096 && snx_ctr_query(p->space, va, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(memory(p, 3, va, 4096, 3, 0) == va && !word(p, va)); /* recycled RAM is zero */
        memory(p, 1, va, 8192, 0, 0);
        assert(used(p) == baseline);
        /* A normal heap allocation can join physically discontiguous holes. */
        memory(p, 3, CTR_HEAP, 3 * 4096, 3, 0);
        memory(p, 1, CTR_HEAP + 4096, 4096, 0, 0);
        uint32_t scatter = CTR_HEAP + 0x10000, first_pa, next_pa;
        memory(p, 3, scatter, 8192, 3, 0);
        assert(snx_ctr_query(p->space, scatter, &first_pa, NULL) == SNX_CTR_MEM_OK);
        assert(snx_ctr_query(p->space, scatter + 4096, &next_pa, NULL) == SNX_CTR_MEM_OK && next_pa != first_pa + 4096);
        assert(snx_ctr_write_ram(p->space, scatter + 4092, "12345678", 8) == SNX_CTR_MEM_OK);
        memory(p, 6, scatter + 4096, 4096, 1, 0);
        assert(snx_ctr_write_ram(p->space, scatter + 4092, "abcdefgh", 8) == SNX_CTR_MEM_PERMISSION);
        assert(word(p, scatter + 4092) == 0x34333231 && word(p, scatter + 4096) == 0x38373635);
        memory(p, 1, scatter, 8192, 0, 0);
        memory(p, 1, CTR_HEAP, 4096, 0, 0);
        memory(p, 1, CTR_HEAP + 8192, 4096, 0, 0);
        assert(used(p) == baseline);
        memory(p, 3, 0, 4096, 3, 0xe0e01bf5u);
        memory(p, 3, CTR_HEAP + 1, 4096, 3, 0xe0e01bf1u);
        memory(p, 3, CTR_HEAP, 4097, 3, 0xe0e01bf2u);
        memory(p, 3, CTR_HEAP, 4096, 4, 0xe0e01beeu);
        memory(p, 3, 0x100000, 4096, 3, 0xe0e01bf5u);
        memory(p, 3, 0xfffff000, 8192, 3, 0xe0e01bf5u);
        memory(p, 3, 0, 0xfffff000, 3, 0xe0e01bfdu);
        memory(p, 0x20003, 0, 4096, 3, 0xe0e01beeu);
        memory(p, 4, CTR_HEAP, 4096, 3, 0xe0e01bf4u);
        memory(p, 3, CTR_HEAP, p->app_limit, 3, 0xc8601bf3u);
        memory(p, 1, 0x100000, 4096, 0, 0xe0e01bf5u);
        assert(used(p) == baseline);
        uint32_t linear = memory(p, 0x10003, 0, 8192, 3, 0), pa;
        assert(snx_ctr_query(p->space, linear, &pa, NULL) == SNX_CTR_MEM_OK);
        assert(linear - CTR_LINEAR == pa - CTR_PHYS_BASE && !word(p, linear));
        memory(p, 0x10003, linear, 4096, 3, 0xe0a01bf5u);
        memory(p, 0x10003, CTR_LINEAR, 4096, 3, 0xe0a01bf5u); /* static physical pages cannot be aliased */
        memory(p, 1, linear, 8192, 0, 0);
        assert(used(p) == baseline);
        r[0] = 3;
        r[1] = CTR_HEAP;
        r[2] = 1;
        r[3] = 4096;
        r[4] = 3;
        assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf1u);
        assert(used(p) == baseline);
        r[1] = 2;
        r[2] = 0;
        assert(snx_ctr_svc(p, 0x2a) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf4u && !r[1] && !r[2]);
        uint32_t handle = create(p);
        assert(handle && object_count(p) == 3);
        r[1] = handle;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t duplicate = r[1];
        assert(duplicate != handle && object_count(p) == 3);
        close_handle(p, handle, 0);
        close_handle(p, handle, INVALID_HANDLE);
        arbitrate(p, handle, 0x300000, 0, UINT32_MAX, 0, SNX_CTR_SVC_RETURN, INVALID_HANDLE);
        arbitrate(p, duplicate, 0x300000, 0, UINT32_MAX, 0, SNX_CTR_SVC_RETURN, 0);
        uint32_t next = create(p);
        assert(next != handle);
        close_handle(p, next, 0);
        close_handle(p, CTR_THREAD_HANDLE, INVALID_HANDLE);
        close_handle(p, CTR_PROCESS_HANDLE, INVALID_HANDLE);
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t thread = r[1];
        arbitrate(p, thread, 0x300000, 0, 1, 0, SNX_CTR_SVC_RETURN, INVALID_HANDLE);
        close_handle(p, thread, 0);
        arbitrate(p, duplicate, 0x300001, 1, 0, 0, SNX_CTR_SVC_RETURN, 0xe0e01bf1u);
        arbitrate(p, duplicate, 0x300000, 5, 0, 0, SNX_CTR_SVC_RETURN, 0xd8e093edu);
        arbitrate(p, duplicate, 0x400000, 1, 0, 0, SNX_CTR_SVC_RETURN, 0xe0e01bf5u);
        put(p, 0x300000, 5);
        arbitrate(p, duplicate, 0x300000, 2, 3, 0, SNX_CTR_SVC_RETURN, 0);
        assert(word(p, 0x300000) == 5);
        arbitrate(p, duplicate, 0x300000, 3, 3, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        arbitrate(p, duplicate, 0x300000, 4, 6, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        assert(word(p, 0x300000) == 4);
        put(p, 0x300000, UINT32_MAX);
        arbitrate(p, duplicate, 0x300000, 4, 0, 0, SNX_CTR_SVC_RETURN, TIMEOUT);
        assert(word(p, 0x300000) == UINT32_MAX - 1);
        arbitrate(p, duplicate, 0x300000, 4, 0, 100, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(word(p, 0x300000) == UINT32_MAX - 1 && !snx_ctr_kernel_blocked(p));
        close_handle(p, duplicate, 0);
        assert(object_count(p) == 2);
        uint32_t handles[CTR_HANDLES];
        for (unsigned i = 0; i < CTR_HANDLES; ++i) {
            r[1] = CTR_THREAD_HANDLE;
            assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
            handles[i] = r[1];
        }
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && r[0] == 0xd8600413u && !r[1]);
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && r[0] == 0xd8600413u && !r[1] && object_count(p) == 2);
        for (unsigned i = 0; i < CTR_HANDLES; ++i) close_handle(p, handles[i], 0);
        unsigned count = 0;
        while (snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && !r[0]) {
            assert(count < CTR_HANDLES);
            handles[count++] = r[1];
        }
        assert(count == CTR_OBJECTS - 3 && !r[1] && object_count(p) == CTR_OBJECTS - 1);
        for (unsigned i = 0; i < count; ++i) close_handle(p, handles[i], 0);
        assert(object_count(p) == 2);
        r[0] = 0xdeadbeef;
        assert(snx_ctr_svc(p, 0x7f) == SNX_CTR_SVC_UNSUPPORTED && r[0] == 0xdeadbeef);
        handle = create(p);
        put(p, 0x300000, 0);
        arbitrate(p, handle, 0x300000, 2, 1, 0, SNX_CTR_SVC_BLOCKED, 0);
        assert(snx_ctr_kernel_blocked(p) && word(p, 0x300000) == UINT32_MAX);
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_BLOCKED);
        SnxCtrModule module = {0};
        uint64_t executed = 99;
        uint32_t pc = r[15];
        assert(snx_ctr_dispatch(&module, p, 10, &executed) == SNX_CTR_RUN_BLOCKED && !executed && r[15] == pc);
        snx_ctr_process_free(p);
    }
    puts(
        "CTR kernel objects, stale handles, resource exhaustion, checked memory and truthful arbitration/waits passed");
    return 0;
}
