/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Serialized, bounded single-core thread contexts; no host threads or JIT.
   SVC register layout from Azahar kernel/svc_wrapper.h (revision in 3DS.md). */
#include "runtime/internal.h"
#include <string.h>
#define THREAD_LIMIT 16u
CtrThread* snx_ctr_thread(SnxCtrProcess* p) { return &p->objects[p->current_thread].data.thread; }
void snx_ctr_object_release(SnxCtrProcess* p, uint32_t id) {
    if (!--p->objects[id].refs) p->objects[id] = (CtrObject){0};
}
void snx_ctr_thread_block(SnxCtrProcess* p, uint32_t id, uint32_t address) {
    CtrThread* t = snx_ctr_thread(p);
    t->waiting_object = id;
    t->waiting_address = address;
    t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    ++p->objects[id].refs;
}
static uint32_t tls(SnxCtrProcess* p, uint32_t* address) {
    unsigned char zero[4096] = {0};
    for (uint32_t slot = 0; slot < THREAD_LIMIT; ++slot) {
        uint32_t va = SNX_CTR_INITIAL_TLS + slot * 512;
        bool used = false;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
            if (p->objects[i].type == CTR_OBJ_THREAD && !p->objects[i].data.thread.dead &&
                p->objects[i].data.thread.context.tpidruro == va)
                used = true;
        if (used) continue;
        uint32_t page = va & ~4095u;
        if (snx_ctr_query(p->space, page, NULL, NULL) != SNX_CTR_MEM_OK) {
            uint32_t physical;
            for (physical = 0; physical < p->app_limit / 4096; ++physical)
                if (!p->physical_owner[physical]) break;
            if (physical == p->app_limit / 4096) return 0xd86007f3u;
            if (snx_ctr_map(p->space, page, CTR_PHYS_BASE + physical * 4096, 4096, 3) != SNX_CTR_MEM_OK)
                return 0xd86007f3u;
            if (snx_ctr_write_ram(p->space, page, zero, sizeof zero) != SNX_CTR_MEM_OK) {
                (void)snx_ctr_unmap(p->space, page, 4096);
                return 0xe0e01bf5u;
            }
            p->physical_owner[physical] = CTR_STATIC;
            *address = va;
            return 0;
        }
        if (snx_ctr_write_ram(p->space, va, zero, 512) != SNX_CTR_MEM_OK) return 0xe0e01bf5u;
        *address = va;
        return 0;
    }
    return 0xc860180cu; /* bounded implementation thread/TLS quota */
}
uint32_t snx_ctr_thread_create(SnxCtrProcess* p, const uint32_t* r, uint32_t* out) {
    *out = 0;
    if (r[0] > 63) return 0xd8e007fdu;
    unsigned char instruction[4];
    uint32_t entry = r[1] & ~1u, width = r[1] & 1 ? 2 : 4;
    if ((entry & (width - 1)) || snx_ctr_fetch(p->space, entry, instruction, width) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    if ((r[3] & 7) || r[3] < 4 || snx_ctr_read_ram(p->space, r[3] - 4, instruction, 4) != SNX_CTR_MEM_OK)
        return 0xe0e01bf5u;
    unsigned permissions;
    if (snx_ctr_query(p->space, r[3] - 4, NULL, &permissions) != SNX_CTR_MEM_OK || !(permissions & SNX_CTR_WRITE))
        return 0xe0e01bf5u;
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_THREAD, out);
    if (result) return result;
    uint32_t id = snx_ctr_handle_object(p, *out), address;
    result = tls(p, &address);
    if (result) {
        snx_ctr_handle_close(p, *out);
        *out = 0;
        return result;
    }
    CtrThread* t = &p->objects[id].data.thread;
    t->context.r[0] = r[2];
    t->context.r[13] = r[3];
    t->context.r[15] = entry;
    t->context.cpsr = 0x10 | (r[1] & 1 ? SNX_ARM_T : 0);
    t->context.tpidruro = address;
    t->priority = r[0];
    ++p->objects[id].refs; /* live thread owns a root, even if its handle closes */
    return 0;
}
static bool ready(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT) return !!o->data.event.signaled;
    if (o->type == CTR_OBJ_THREAD) return !!o->data.thread.dead;
    return o->type == CTR_OBJ_MUTEX && (!o->data.mutex.depth || o->data.mutex.owner == thread);
}
static void acquire(SnxCtrProcess* p, uint32_t id, uint32_t thread) {
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT && !o->data.event.reset_type) o->data.event.signaled = 0;
    if (o->type == CTR_OBJ_MUTEX) {
        o->data.mutex.owner = thread;
        ++o->data.mutex.depth;
    }
}
SnxCtrSvcResult snx_ctr_wait_many(SnxCtrProcess* p, uint32_t* r) {
    if (!r[2] || r[2] > 64 || r[3] > 1) {
        r[0] = 0xd8e007fdu;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    unsigned char bytes[256];
    uint32_t ids[64], count = r[2], found = UINT32_MAX;
    if (snx_ctr_read_ram(p->space, r[1], bytes, count * 4) != SNX_CTR_MEM_OK) {
        r[0] = 0xe0e01bf5u;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    bool all = true;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char* b = bytes + i * 4;
        uint32_t handle = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        ids[i] = snx_ctr_handle_object(p, handle);
        if (!ids[i]) {
            r[0] = 0xd8e007f7u;
            r[1] = UINT32_MAX;
            return SNX_CTR_SVC_RETURN;
        }
        uint32_t type = p->objects[ids[i]].type;
        if (type != CTR_OBJ_EVENT && type != CTR_OBJ_MUTEX && type != CTR_OBJ_THREAD) return SNX_CTR_SVC_UNSUPPORTED;
        for (uint32_t j = 0; j < i; ++j)
            if (ids[i] == ids[j]) return SNX_CTR_SVC_UNSUPPORTED;
        if (type == CTR_OBJ_MUTEX && p->objects[ids[i]].data.mutex.depth == UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
        bool available = ready(p, ids[i], p->current_thread);
        if (available && found == UINT32_MAX) found = i;
        all = all && available;
    }
    if (r[3] ? all : found != UINT32_MAX) {
        for (uint32_t i = 0; i < count; ++i)
            if (r[3] || i == found) acquire(p, ids[i], p->current_thread);
        r[1] = r[3] ? UINT32_MAX : found;
        r[0] = 0;
        return SNX_CTR_SVC_RETURN;
    }
    if (!r[0] && !r[4]) {
        r[0] = 0x09401bfeu;
        r[1] = UINT32_MAX;
        return SNX_CTR_SVC_RETURN;
    }
    if (r[0] != UINT32_MAX || r[4] != UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
    CtrThread* t = snx_ctr_thread(p);
    for (uint32_t i = 0; i < count; ++i) ++p->objects[ids[i]].refs;
    t->waiting_object = ids[0];
    t->wait_multiple = 1;
    t->wait_all = !!r[3];
    t->wait_more_count = count - 1;
    for (uint32_t i = 1; i < count; ++i) t->wait_more[i - 1] = ids[i];
    return SNX_CTR_SVC_BLOCKED;
}
void snx_ctr_wake_waiters(SnxCtrProcess* p, uint32_t object) {
    /* Highest-priority eligible waiter first. One-shot events and mutexes are
       acquired on completion; later waiters recheck the resulting state. */
    for (;;) {
        uint32_t best = 0, index = 0;
        for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
            if (p->objects[i].type != CTR_OBJ_THREAD) continue;
            CtrThread* t = &p->objects[i].data.thread;
            if (!t->waiting_object || t->dead) continue;
            bool includes = t->waiting_object == object, all = true;
            uint32_t first = UINT32_MAX;
            for (uint32_t j = 0; j <= t->wait_more_count; ++j) {
                uint32_t id = j ? t->wait_more[j - 1] : t->waiting_object;
                includes = includes || id == object;
                bool available = ready(p, id, i);
                if (available && first == UINT32_MAX) first = j;
                all = all && available;
            }
            if (!includes || (t->wait_all ? !all : first == UINT32_MAX)) continue;
            if (!best || t->priority < p->objects[best].data.thread.priority) {
                best = i;
                index = first;
            }
        }
        if (!best) return;
        CtrThread* t = &p->objects[best].data.thread;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            if (t->wait_all || j == index) acquire(p, j ? t->wait_more[j - 1] : t->waiting_object, best);
        t->context.r[0] = 0;
        if (t->wait_multiple) t->context.r[1] = t->wait_all ? UINT32_MAX : index;
        t->context.exit = t->context.syndrome = 0;
        for (uint32_t j = 0; j <= t->wait_more_count; ++j)
            snx_ctr_object_release(p, j ? t->wait_more[j - 1] : t->waiting_object);
        t->waiting_object = t->waiting_address = t->wait_more_count = t->wait_multiple = t->wait_all = 0;
    }
}
bool snx_ctr_schedule(SnxCtrProcess* p) {
    uint32_t next = 0;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
        if (p->objects[i].type != CTR_OBJ_THREAD) continue;
        CtrThread* t = &p->objects[i].data.thread;
        if (t->dead || t->waiting_object) continue;
        if (!next || t->priority < p->objects[next].data.thread.priority ||
            (t->priority == p->objects[next].data.thread.priority && i == p->current_thread))
            next = i;
    }
    if (!next) return false;
    if (next != p->current_thread) {
        /* Only the plain CPU prefix is saved. The checked adapter/callback
           context never moves; exception/context switch loses exclusives. */
        p->cpu.state.cpu.exclusive_valid = 0;
        snx_ctr_thread(p)->context = p->cpu.state.cpu;
        p->current_thread = next;
        p->cpu.state.cpu = snx_ctr_thread(p)->context;
        p->cpu.state.cpu.exclusive_valid = 0;
    }
    return true;
}
