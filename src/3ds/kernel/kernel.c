/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR SVC ABI/result/arbiter semantics referenced against Citra/Azahar
   (GPL-2.0-or-later), src/core/hle/kernel/{svc,handle_table,address_arbiter}.cpp
   and errors.h at 68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e.
   Independent serialized implementation; no reference runtime dependency. */
#include "ctr_kernel.h"
#include "runtime/internal.h"
#include <stdlib.h>
#include "services/gsp.h"
#include <string.h>
#include "common.h"

#define OK                  0u
#define INVALID_HANDLE      0xd8e007f7u
#define OUT_OF_HANDLES      0xd8600413u
#define OUT_OF_MEMORY       0xd86007f3u
#define OUT_OF_HEAP         0xc8601bf3u
#define INVALID_ADDRESS     0xe0e01bf5u
#define MISALIGNED_ADDRESS  0xe0e01bf1u
#define MISALIGNED_SIZE     0xe0e01bf2u
#define INVALID_SIZE        0xe0e01bfdu
#define INVALID_PERMISSION  0xe0e01beeu
#define INVALID_COMBINATION 0xe0e01beeu
#define INVALID_ENUM        0xd8e093edu
#define INVALID_STATE       0xe0a01bf5u
#define NOT_IMPLEMENTED     0xe0e01bf4u
#define TIMED_OUT           0x09401bfeu

uint32_t snx_ctr_handle_object(SnxCtrProcess* p, uint32_t handle) {
    if (handle == CTR_PROCESS_HANDLE) return 1;
    if (handle == CTR_THREAD_HANDLE) return p->current_thread;
    uint32_t slot = handle & 511, generation = handle >> 9;
    if (!slot || slot > CTR_HANDLES || !generation) return 0;
    CtrHandle* h = &p->handles[slot - 1];
    return h->generation == generation ? h->object : 0;
}
uint32_t snx_ctr_handle_add(SnxCtrProcess* p, uint32_t id, uint32_t* out) {
    *out = 0;
    if (!id || id >= CTR_OBJECTS || !p->objects[id].type) return INVALID_HANDLE;
    for (unsigned i = 0; i < CTR_HANDLES; ++i) {
        CtrHandle* h = &p->handles[i];
        if (h->object) continue;
        h->generation = h->generation % 65535 + 1;
        h->object = id;
        ++p->objects[id].refs;
        *out = (h->generation << 9) | (i + 1);
        return OK;
    }
    return OUT_OF_HANDLES;
}
uint32_t snx_ctr_object_create(SnxCtrProcess* p, uint32_t type, uint32_t* out) {
    *out = 0;
    for (unsigned i = 3; i < CTR_OBJECTS; ++i) {
        if (p->objects[i].type) continue;
        p->objects[i].type = type;
        uint32_t result = snx_ctr_handle_add(p, i, out);
        if (result) p->objects[i] = (CtrObject){0};
        return result;
    }
    return OUT_OF_MEMORY;
}
uint32_t snx_ctr_handle_close(SnxCtrProcess* p, uint32_t handle) {
    uint32_t id = snx_ctr_handle_object(p, handle);
    if (!id || handle == CTR_PROCESS_HANDLE || handle == CTR_THREAD_HANDLE) return INVALID_HANDLE;
    p->handles[(handle & 511) - 1].object = 0;
    snx_ctr_object_release(p, id);
    return OK;
}
static uint32_t read32(SnxCtrProcess* p, uint32_t va, uint32_t* out) {
    unsigned char b[4];
    if (snx_ctr_read_ram(p->space, va, b, 4) != SNX_CTR_MEM_OK) return INVALID_ADDRESS;
    *out = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return OK;
}
static uint32_t write32(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (8 * i));
    return snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK ? OK : INVALID_ADDRESS;
}
static bool available(SnxCtrProcess* p, uint32_t va, uint32_t size) {
    for (uint32_t off = 0; off < size; off += 4096)
        if (snx_ctr_query(p->space, va + off, NULL, NULL) == SNX_CTR_MEM_OK) return false;
    return true;
}
/* QueryMemory's coarse class for one page: 0 free, 2 IO, 3 static, 4 code,
   5 private RAM, 6 a kernel-registered shared block. Permissions come from
   the actual mapping. */
static void query_class(SnxCtrProcess* p, uint32_t page, uint32_t* state, uint32_t* perm) {
    *state = 0;
    *perm = 0;
    uint32_t pa = 0;
    unsigned perms = 0;
    SnxCtrMemResult result = snx_ctr_query(p->space, page, &pa, &perms);
    if (result == SNX_CTR_MEM_UNMAPPED) return;
    if (result != SNX_CTR_MEM_OK) {
        *state = 2;
        return;
    }
    *perm = perms;
    for (unsigned i = 0; i < 8; ++i) {
        CtrSharedMap* m = &p->shared_maps[i];
        if (m->object && page >= m->va && (uint64_t)page < (uint64_t)m->va + m->size) {
            *state = 6;
            return;
        }
    }
    *state = pa >= CTR_PHYS_BASE ? (perms & 4 ? 4u : perms & 2 ? 5u : 3u) : 2u;
}
/* Heap/linear commit, free and protect. MAP/UNMAP aliases are deliberately not
   implemented; no mappings, ownership or accounting change on rejected calls. */
uint32_t snx_ctr_kernel_memory(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t source, uint32_t size,
                               uint32_t permissions, uint32_t* output) {
    *output = 0;
    if ((va | source) & 4095) return MISALIGNED_ADDRESS;
    if (size & 4095) return MISALIGNED_SIZE;
    if (permissions & ~3u) return INVALID_PERMISSION;
    if (op & ~(0x10000u | 0xffu)) return INVALID_COMBINATION;
    bool linear = !!(op & 0x10000);
    op &= 0xff;
    if (linear && op != 3) return INVALID_COMBINATION;
    if (op == 4 || op == 5) return NOT_IMPLEMENTED;
    if (op != 1 && op != 3 && op != 6) return INVALID_COMBINATION;
    if (!size) {
        *output = va;
        return OK;
    }
    uint32_t base = linear ? CTR_LINEAR : CTR_HEAP;
    uint32_t end = linear ? CTR_LINEAR + p->app_limit : CTR_HEAP_END;
    if (op != 3) {
        if (va >= CTR_LINEAR && va < CTR_LINEAR + p->app_limit) {
            base = CTR_LINEAR;
            end = CTR_LINEAR + p->app_limit;
            linear = true;
        }
    }
    if (size > end - base) return INVALID_SIZE;
    if (va && (va < base || (uint64_t)va + size > end)) return INVALID_ADDRESS;
    if (op == 3) {
        if (!va && !linear) return INVALID_ADDRESS;
        if (va && !available(p, va, size)) return INVALID_STATE;
        uint32_t pages = size / 4096, total = p->app_limit / 4096, first = 0, run = 0;
        uint32_t* plan = malloc(pages * sizeof *plan);
        if (!plan) return OUT_OF_MEMORY;
        if (linear) {
            if (va) {
                first = (va - CTR_LINEAR) / 4096;
                for (uint32_t i = first; i < first + pages; ++i)
                    if (p->physical_owner[i]) {
                        free(plan);
                        return INVALID_STATE;
                    }
            } else {
                for (uint32_t i = 0; i < total; ++i) {
                    if (p->physical_owner[i])
                        run = 0;
                    else if (++run == pages) {
                        first = i + 1 - pages;
                        break;
                    }
                }
                if (run != pages) {
                    free(plan);
                    return OUT_OF_HEAP;
                }
            }
            va = CTR_LINEAR + first * 4096;
            for (uint32_t i = 0; i < pages; ++i) plan[i] = first + i;
        } else {
            /* Ordinary heap pages need not be physically contiguous. */
            for (uint32_t i = 0; i < total && run < pages; ++i)
                if (!p->physical_owner[i]) plan[run++] = i;
            if (run != pages) {
                free(plan);
                return OUT_OF_HEAP;
            }
        }
        if (!available(p, va, size)) {
            free(plan);
            return INVALID_STATE;
        }
        uint32_t mapped = 0;
        while (mapped < pages) {
            uint32_t count = 1;
            while (mapped + count < pages && plan[mapped + count] == plan[mapped] + count) ++count;
            if (snx_ctr_map(p->space, va + mapped * 4096, CTR_PHYS_BASE + plan[mapped] * 4096, count * 4096,
                            SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
                if (mapped) snx_ctr_unmap(p->space, va, mapped * 4096);
                free(plan);
                return OUT_OF_MEMORY;
            }
            mapped += count;
        }
        unsigned char zero[4096] = {0};
        for (uint32_t off = 0; off < size; off += 4096) {
            if (snx_ctr_write_ram(p->space, va + off, zero, sizeof zero) != SNX_CTR_MEM_OK) {
                snx_ctr_unmap(p->space, va, size);
                free(plan);
                return INVALID_STATE;
            }
        }
        if (snx_ctr_protect(p->space, va, size, permissions) != SNX_CTR_MEM_OK) {
            snx_ctr_unmap(p->space, va, size);
            free(plan);
            return INVALID_STATE;
        }
        for (uint32_t i = 0; i < pages; ++i) p->physical_owner[plan[i]] = linear ? CTR_LINEAR_PAGE : CTR_HEAP_PAGE;
        free(plan);
    } else {
        if (!va) return INVALID_ADDRESS;
        uint32_t pages = size / 4096;
        uint32_t* physical = malloc(pages * sizeof *physical);
        if (!physical) return OUT_OF_MEMORY;
        for (uint32_t i = 0; i < pages; ++i) {
            uint32_t pa;
            if (snx_ctr_query(p->space, va + i * 4096, &pa, NULL) != SNX_CTR_MEM_OK || pa < CTR_PHYS_BASE ||
                pa >= p->physical_end ||
                p->physical_owner[(pa - CTR_PHYS_BASE) / 4096] != (linear ? CTR_LINEAR_PAGE : CTR_HEAP_PAGE)) {
                free(physical);
                return INVALID_STATE;
            }
            physical[i] = (pa - CTR_PHYS_BASE) / 4096;
        }
        SnxCtrMemResult result =
            op == 1 ? snx_ctr_unmap(p->space, va, size) : snx_ctr_protect(p->space, va, size, permissions);
        if (result != SNX_CTR_MEM_OK) {
            free(physical);
            return INVALID_STATE;
        }
        if (op == 1) {
            for (uint32_t i = 0; i < pages; ++i) p->physical_owner[physical[i]] = CTR_UNUSED;
        }
        free(physical);
    }
    p->cpu.state.cpu.exclusive_valid = 0;
    *output = va;
    return OK;
}
static SnxCtrSvcResult arbitrate(SnxCtrProcess* p, SnxArmCPU* cpu) {
    uint32_t* r = cpu->r;
    uint32_t id = snx_ctr_handle_object(p, r[0]), va = r[1], type = r[2], threshold = r[3], value;
    if (!id || p->objects[id].type != CTR_OBJ_ARBITER) {
        r[0] = INVALID_HANDLE;
        return SNX_CTR_SVC_RETURN;
    }
    if (type > 4) {
        r[0] = INVALID_ENUM;
        return SNX_CTR_SVC_RETURN;
    }
    if (va & 3) {
        r[0] = MISALIGNED_ADDRESS;
        return SNX_CTR_SVC_RETURN;
    }
    if (type == 0) {
        uint32_t remaining = threshold & 0x80000000u ? UINT32_MAX : threshold;
        while (remaining) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
                if (p->objects[i].type != CTR_OBJ_THREAD) continue;
                CtrThread* t = &p->objects[i].data.thread;
                if (t->waiting_object == id && t->waiting_address == va &&
                    (!best || t->priority < p->objects[best].data.thread.priority))
                    best = i;
            }
            if (!best) break;
            CtrThread* t = &p->objects[best].data.thread;
            t->waiting_object = t->waiting_address = 0;
            t->context.r[0] = t->context.exit = t->context.syndrome = 0;
            snx_ctr_object_release(p, id);
            --remaining;
        }
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    uint32_t result = read32(p, va, &value);
    if (result) {
        r[0] = result;
        return SNX_CTR_SVC_RETURN;
    }
    /* CTR's timed variants return Timeout even when the predicate does not
       suspend the thread, matching the reference kernel-facing behavior. */
    if ((value ^ 0x80000000u) >= (threshold ^ 0x80000000u)) {
        r[0] = type >= 3 ? TIMED_OUT : OK;
        return SNX_CTR_SVC_RETURN;
    }
    /* Finite nonzero deadlines need an actual timer. Refuse before the
       decrement or wait publication, rather than turn them into infinity. */
    if (type >= 3 && (r[4] || r[5]) && (r[4] != UINT32_MAX || r[5] != UINT32_MAX)) return SNX_CTR_SVC_UNSUPPORTED;
    if (type == 2 || type == 4) {
        result = write32(p, va, value - 1);
        if (result) {
            r[0] = result;
            return SNX_CTR_SVC_RETURN;
        }
    }
    if (type >= 3 && !r[4] && !r[5]) {
        r[0] = TIMED_OUT;
        return SNX_CTR_SVC_RETURN;
    }
    snx_ctr_thread_block(p, id, va);
    return SNX_CTR_SVC_BLOCKED;
}
static SnxCtrSvcResult wait_one(SnxCtrProcess* p, uint32_t* r) {
    uint32_t id = snx_ctr_handle_object(p, r[0]);
    if (!id) {
        r[0] = INVALID_HANDLE;
        return SNX_CTR_SVC_RETURN;
    }
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT && o->data.event.signaled) {
        if (!o->data.event.reset_type) o->data.event.signaled = 0;
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    /* Waits on the GSP interrupt event complete at real host-paced vblank
       deliveries, not at arbitrary dispatch calls. */
    if (o->type == CTR_OBJ_EVENT && snx_ctr_gsp_event(p, id)) return snx_ctr_gsp_wait(p, id, r);
    if (o->type == CTR_OBJ_THREAD && o->data.thread.dead) {
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    if (o->type != CTR_OBJ_MUTEX && o->type != CTR_OBJ_EVENT && o->type != CTR_OBJ_THREAD)
        return SNX_CTR_SVC_UNSUPPORTED;
    if (o->type == CTR_OBJ_EVENT || o->type == CTR_OBJ_THREAD ||
        (o->data.mutex.depth && o->data.mutex.owner != p->current_thread)) {
        if (!r[2] && !r[3]) {
            r[0] = TIMED_OUT;
            return SNX_CTR_SVC_RETURN;
        }
        if (r[2] != UINT32_MAX || r[3] != UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
        snx_ctr_thread_block(p, id, 0);
        return SNX_CTR_SVC_BLOCKED;
    }
    if (o->data.mutex.depth == UINT32_MAX)
        r[0] = INVALID_STATE;
    else {
        o->data.mutex.owner = p->current_thread;
        ++o->data.mutex.depth;
        r[0] = OK;
    }
    return SNX_CTR_SVC_RETURN;
}
bool snx_ctr_kernel_blocked(const SnxCtrProcess* p) {
    return p && p->objects[p->current_thread].data.thread.waiting_object;
}
SnxCtrSvcResult snx_ctr_svc(SnxCtrProcess* p, uint32_t number) {
    if (!p) return SNX_CTR_SVC_UNSUPPORTED;
    if (snx_ctr_kernel_blocked(p)) return SNX_CTR_SVC_BLOCKED;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    uint32_t* r = cpu->r;
    p->last_svc = number;
    ++p->svc_count;
    cpu->exclusive_valid = 0; /* exception entry may clear the local monitor */
    switch (number) {
        case 0x01: {
            uint32_t out;
            uint32_t result = snx_ctr_kernel_memory(p, r[0], r[1], r[2], r[3], r[4], &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x02: {
            /* QueryMemory: the real region containing r2. Coarse state classes
               derive from the actual mapping: free/shared/private/code/static/
               IO. Region walks stay inside the user VA window [0, 0x20000000). */
            if ((r[0] | r[1] | r[2]) & 3) {
                r[0] = MISALIGNED_ADDRESS;
                return SNX_CTR_SVC_RETURN;
            }
            uint32_t base = r[2] & ~4095u, end = base + 4096, first = 0, first_perm = 0;
            unsigned char info[16] = {0}, page_info[4] = {0};
            query_class(p, base, &first, &first_perm);
            while (base >= 4096) {
                uint32_t state = 0, perm = 0;
                query_class(p, base - 4096, &state, &perm);
                if (state != first || perm != first_perm) break;
                base -= 4096;
            }
            while (end < 0x20000000u) {
                uint32_t state = 0, perm = 0;
                query_class(p, end, &state, &perm);
                if (state != first || perm != first_perm) break;
                end += 4096;
            }
            for (unsigned i = 0; i < 4; ++i) {
                info[i] = (unsigned char)(base >> (i * 8));
                info[4 + i] = (unsigned char)((end - base) >> (i * 8));
                info[8 + i] = (unsigned char)(first_perm >> (i * 8));
                info[12 + i] = (unsigned char)(first >> (i * 8));
            }
            if (snx_ctr_write_ram(p->space, r[0], info, sizeof info) != SNX_CTR_MEM_OK ||
                snx_ctr_write_ram(p->space, r[1], page_info, sizeof page_info) != SNX_CTR_MEM_OK)
                r[0] = INVALID_ADDRESS;
            else
                r[0] = OK;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x03: return SNX_CTR_SVC_EXIT;
        case 0x08: {
            if (r[4] != 0xfffffffeu && r[4] != 0) return SNX_CTR_SVC_UNSUPPORTED;
            uint32_t out = 0, result = snx_ctr_thread_create(p, r, &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x14: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_MUTEX)
                r[0] = INVALID_HANDLE;
            else if (p->objects[id].data.mutex.owner != p->current_thread || !p->objects[id].data.mutex.depth)
                r[0] = 0xd8e0041fu;
            else {
                if (!--p->objects[id].data.mutex.depth) {
                    p->objects[id].data.mutex.owner = 0;
                    snx_ctr_wake_waiters(p, id);
                }
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x17: {
            uint32_t out = 0;
            if (r[1] > 1) return SNX_CTR_SVC_UNSUPPORTED;
            uint32_t reset = r[1], result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &out);
            if (!result) p->objects[snx_ctr_handle_object(p, out)].data.event.reset_type = reset;
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x18: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_EVENT)
                r[0] = INVALID_HANDLE;
            else {
                p->objects[id].data.event.signaled = 1;
                snx_ctr_wake_waiters(p, id);
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x19: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_EVENT)
                r[0] = INVALID_HANDLE;
            else {
                p->objects[id].data.event.signaled = 0;
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1f: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_SHARED)
                r[0] = INVALID_HANDLE;
            else {
                CtrObject* o = &p->objects[id];
                if ((r[1] & 4095) || r[1] < 0x10000000 || (uint64_t)r[1] + o->data.shared.size > 0x1f800000)
                    r[0] = INVALID_ADDRESS;
                else if (r[2] != o->data.shared.permissions || (r[3] != 0 && r[3] != 0x10000000))
                    return SNX_CTR_SVC_UNSUPPORTED;
                else {
                    unsigned slot = 8, replace = 8;
                    for (unsigned i = 0; i < 8; ++i) {
                        if (p->shared_maps[i].object == id) replace = i;
                        if (slot == 8 && !p->shared_maps[i].object) slot = i;
                    }
                    if (replace == 8 && slot == 8) {
                        r[0] = OUT_OF_MEMORY;
                        return SNX_CTR_SVC_RETURN;
                    }
                    SnxCtrMemResult mapped =
                        snx_ctr_map(p->space, r[1], o->data.shared.physical, o->data.shared.size, r[2]);
                    r[0] = mapped == SNX_CTR_MEM_OK ? OK : mapped == SNX_CTR_MEM_NOMEM ? OUT_OF_MEMORY : INVALID_STATE;
                    if (mapped == SNX_CTR_MEM_OK) {
                        unsigned slot2 = replace == 8 ? slot : replace;
                        p->shared_maps[slot2] = (CtrSharedMap){r[1], o->data.shared.size, id};
                    }
                }
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x21: {
            uint32_t out = 0, result = snx_ctr_object_create(p, CTR_OBJ_ARBITER, &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x22: return arbitrate(p, cpu);
        case 0x23: r[0] = snx_ctr_handle_close(p, r[0]); return SNX_CTR_SVC_RETURN;
        case 0x24: return wait_one(p, r);
        case 0x25: return snx_ctr_wait_many(p, r);
        case 0x27: {
            uint32_t id = snx_ctr_handle_object(p, r[1]), out = 0;
            r[0] = id ? snx_ctr_handle_add(p, id, &out) : INVALID_HANDLE;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x28: {
            double elapsed = snx_seconds() - p->clock_origin;
            if (!(elapsed >= 0 && elapsed < 0x1p64 / 268111856.0)) return SNX_CTR_SVC_UNSUPPORTED;
            uint64_t ticks = (uint64_t)(elapsed * 268111856.0);
            r[0] = (uint32_t)ticks;
            r[1] = (uint32_t)(ticks >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x2a: {
            uint32_t type = r[1], param = r[2];
            uint64_t value = 0;
            uint32_t result = OK;
            if (type == 0 && (param == 0 || param == 1)) {
                for (uint32_t i = 0; i < p->app_limit / 4096; ++i)
                    if (p->physical_owner[i]) value += 4096;
            } else if (type == 0 && (param == 2 || param == 3))
                value = 0; /* no system/base allocations */
            else
                result = NOT_IMPLEMENTED;
            r[0] = result;
            r[1] = (uint32_t)value;
            r[2] = (uint32_t)(value >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x2d: {
            uint32_t out = 0, result = snx_ctr_named_connect(p, r[1], &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x32: {
            uint32_t result = 0;
            SnxCtrSvcResult state = snx_ctr_ipc_send(p, r[0], &result);
            if (state == SNX_CTR_SVC_RETURN) r[0] = result;
            return state;
        }
        case 0x35: {
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            r[0] = id && p->objects[id].type == CTR_OBJ_PROCESS ? 0 : INVALID_HANDLE;
            r[1] = r[0] ? 0 : CTR_PROCESS_ID;
            return SNX_CTR_SVC_RETURN;
        }
        default: return SNX_CTR_SVC_UNSUPPORTED;
    }
}
