/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aot/ctr_module.h"
static uint32_t event(SnxCtrProcess* p, uint32_t reset) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[1] = reset;
    assert(snx_ctr_svc(p, 0x17) == SNX_CTR_SVC_RETURN && !r[0]);
    return r[1];
}
static void word(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (8 * i));
    assert(snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 4096,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        SnxArmCPU* cpu = &p->cpu.state.cpu;
        uint32_t* r = cpu->r;
        r[0] = 49;
        r[1] = 0x100004;
        r[2] = 37;
        r[3] = SNX_CTR_STACK_TOP - 0x1000;
        r[4] = 0xfffffffe;
        assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t thread = r[1], tid = snx_ctr_handle_object(p, thread);
        assert(tid && tid != 2 && p->objects[tid].refs == 2 && snx_ctr_schedule(p) && p->current_thread == 2);
        uint32_t h = event(p, 0), eid = snx_ctr_handle_object(p, h), duplicate;
        assert(!snx_ctr_handle_add(p, eid, &duplicate));
        cpu->tpidrurw = 0x12345678;
        cpu->d[7] = UINT64_C(0x12345678abcdef01);
        cpu->fpscr = 0x80000000;
        r[5] = 0x55555555;
        r[0] = h;
        r[2] = r[3] = UINT32_MAX;
        cpu->exit = SNX_ARM_SVC;
        cpu->syndrome = 0x24;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && p->objects[eid].refs == 3);
        uint64_t calls = p->svc_count;
        assert(snx_ctr_svc(p, 3) == SNX_CTR_SVC_BLOCKED && p->svc_count == calls);
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        assert(r[0] == 37 && r[15] == 0x100004 && cpu->tpidruro == SNX_CTR_INITIAL_TLS + 512);
        assert(!cpu->mem && !cpu->mem_size && p->cpu.state.context == &p->cpu);
        assert(!snx_ctr_handle_close(p, h) && p->objects[eid].refs == 2); /* wait owns its own reference */
        r[0] = duplicate;
        assert(snx_ctr_svc(p, 0x18) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!p->objects[eid].data.event.signaled && p->objects[eid].refs == 1);
        uint32_t mutex;
        assert(!snx_ctr_object_create(p, CTR_OBJ_MUTEX, &mutex));
        uint32_t mid = snx_ctr_handle_object(p, mutex);
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = mutex;
            r[2] = r[3] = UINT32_MAX;
            assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        assert(snx_ctr_schedule(p) && p->current_thread == 2);
        assert(!r[0] && r[5] == 0x55555555 && !cpu->exit && !cpu->syndrome && !cpu->exclusive_valid);
        assert(cpu->tpidruro == SNX_CTR_INITIAL_TLS && cpu->tpidrurw == 0x12345678 && cpu->fpscr == 0x80000000);
        assert(cpu->d[7] == UINT64_C(0x12345678abcdef01));
        assert(!snx_ctr_handle_close(p, duplicate) && !p->objects[eid].type);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e0041f &&
               p->objects[mid].data.mutex.depth == 2);
        r[0] = mutex;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(p->objects[2].data.thread.waiting_object == mid && p->objects[mid].data.mutex.depth == 1);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(p->objects[mid].data.mutex.owner == 2 && p->objects[mid].data.mutex.depth == 1);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0]);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!snx_ctr_handle_close(p, mutex));
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t arbiter = r[1];
        word(p, 0x300300, 0);
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = r[3] = 1;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = 0;
        r[3] = 1;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_RETURN && !r[0] && snx_ctr_schedule(p) && p->current_thread == 2);
        assert(!r[0] && !snx_ctr_kernel_blocked(p) && !snx_ctr_handle_close(p, arbiter));
        uint32_t first = event(p, 0), second = event(p, 0);
        uint32_t first_id = snx_ctr_handle_object(p, first), second_id = snx_ctr_handle_object(p, second);
        word(p, 0x300200, first);
        word(p, 0x300204, second);
        p->objects[first_id].data.event.signaled = 1;
        r[0] = UINT32_MAX;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 1;
        r[4] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_BLOCKED && p->objects[first_id].data.event.signaled);
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = second;
        assert(snx_ctr_svc(p, 0x18) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!p->objects[first_id].data.event.signaled && !p->objects[second_id].data.event.signaled);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0] && r[1] == UINT32_MAX);
        p->objects[first_id].data.event.signaled = 1;
        word(p, 0x300204, UINT32_MAX);
        r[0] = 0;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 0;
        r[4] = 0;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007f7);
        assert(p->objects[first_id].data.event.signaled); /* validate all handles before consumption */
        r[0] = second;
        r[1] = UINT32_MAX;
        r[2] = r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && r[0] == 0x09401bfe);
        r[0] = second;
        r[2] = 100;
        r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_UNSUPPORTED && !snx_ctr_kernel_blocked(p));
        word(p, 0x300204, second);
        r[0] = 100;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 1;
        r[4] = 0;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_UNSUPPORTED && p->objects[first_id].data.event.signaled);
        assert(!snx_ctr_handle_close(p, thread) && p->objects[tid].refs == 1); /* live thread root */
        r[0] = second;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = second;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && !snx_ctr_schedule(p));
        SnxArmCPU frozen = *cpu;
        const SnxCtrModule empty = {0};
        uint64_t executed;
        assert(snx_ctr_dispatch(&empty, p, 10, &executed) == SNX_CTR_RUN_BLOCKED && !executed);
        assert(!memcmp(&frozen, cpu, sizeof frozen));
        snx_ctr_process_free(p);
        p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        r = p->cpu.state.cpu.r;
        uint32_t heap;
        assert(!snx_ctr_kernel_memory(p, 3, 0x08000000, 0, 4096, 3, &heap));
        unsigned char recycled[4096];
        memset(recycled, 0xaa, sizeof recycled);
        assert(snx_ctr_write_ram(p->space, heap, recycled, sizeof recycled) == SNX_CTR_MEM_OK);
        assert(!snx_ctr_kernel_memory(p, 1, heap, 0, 4096, 0, &heap));
        for (unsigned i = 1; i < 16; ++i) {
            r[0] = 49;
            r[1] = 0x100004;
            r[2] = i;
            r[3] = SNX_CTR_STACK_TOP;
            r[4] = 0xfffffffe;
            assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && !r[0]);
            tid = snx_ctr_handle_object(p, r[1]);
            assert(p->objects[tid].data.thread.context.tpidruro == SNX_CTR_INITIAL_TLS + i * 512);
            assert(!snx_ctr_handle_close(p, r[1]) && p->objects[tid].refs == 1);
            if (i == 8) {
                assert(snx_ctr_read_ram(p->space, SNX_CTR_INITIAL_TLS + 4096, recycled, sizeof recycled) ==
                       SNX_CTR_MEM_OK);
                for (unsigned j = 0; j < sizeof recycled; ++j) assert(!recycled[j]);
            }
        }
        r[0] = 49;
        r[1] = 0x100004;
        r[2] = 0;
        r[3] = SNX_CTR_STACK_TOP;
        r[4] = 0xfffffffe;
        assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && r[0] == 0xc860180c && !r[1]);
        unsigned objects = 0;
        for (unsigned i = 1; i < CTR_OBJECTS; ++i) objects += !!p->objects[i].type;
        assert(objects == 17); /* no leaked partially created seventeenth thread */
        snx_ctr_process_free(p);
    }
    puts(
        "Serialized native thread contexts/TLS, recursive wait ownership, real event wakeups and atomic wait-all passed");
    return 0;
}
