/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_memory.h"
#include <stdlib.h>
#include <string.h>

/* Two-level ARM-sized page metadata, not an emulated hardware MMU table. */
typedef struct {
    uint32_t pa;
    unsigned char permissions, present;
} Page;
struct SnxCtrMemory {
    const SnxCtrProfile* profile;
    unsigned char* backing[8];
    void* mmio_context;
    SnxCtrMmioRead read;
    SnxCtrMmioWrite write;
    uint64_t epoch;
};
struct SnxCtrAddressSpace {
    SnxCtrMemory* memory;
    Page* tables[1024];
};

SnxCtrMemory* snx_ctr_memory_new(SnxCtrModel model) {
    const SnxCtrProfile* p = snx_ctr_profile(model);
    if (!p || p->region_count > 8) return NULL;
    SnxCtrMemory* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->profile = p;
    m->epoch = 1;
    for (size_t i = 0; i < p->region_count; ++i) {
        if (p->regions[i].kind == SNX_CTR_MMIO) continue;
        m->backing[i] = calloc(1, p->regions[i].size);
        if (!m->backing[i]) {
            snx_ctr_memory_free(m);
            return NULL;
        }
    }
    return m;
}
void snx_ctr_memory_free(SnxCtrMemory* m) {
    if (!m) return;
    for (size_t i = 0; i < 8; ++i) free(m->backing[i]);
    free(m);
}
void snx_ctr_memory_mmio(SnxCtrMemory* m, void* context, SnxCtrMmioRead read, SnxCtrMmioWrite write) {
    if (!m) return;
    m->mmio_context = context;
    m->read = read;
    m->write = write;
}
SnxCtrAddressSpace* snx_ctr_space_new(SnxCtrMemory* m) {
    if (!m) return NULL;
    SnxCtrAddressSpace* s = calloc(1, sizeof *s);
    if (s) s->memory = m;
    return s;
}
void snx_ctr_space_free(SnxCtrAddressSpace* s) {
    if (!s) return;
    for (size_t i = 0; i < 1024; ++i) free(s->tables[i]);
    free(s);
}
static Page* page(const SnxCtrAddressSpace* s, uint32_t address) {
    Page* table = s->tables[address >> 22];
    return table ? &table[(address >> 12) & 1023] : NULL;
}
/* A board-wide epoch conservatively invalidates every reservation on any
   successful store or mapping change, including aliases and other spaces.
   Saturation disables exclusive success rather than permitting an ABA wrap. */
static void changed(SnxCtrMemory* m) {
    if (m->epoch != UINT64_MAX) ++m->epoch;
}
uint64_t snx_ctr_memory_epoch(const SnxCtrAddressSpace* s) { return s ? s->memory->epoch : 0; }
static bool valid_range(uint32_t va, uint32_t size) {
    return size && !((va | size) & 4095) && (uint64_t)va + size <= UINT64_C(0x100000000);
}
static bool permissions_valid(unsigned permissions) { return !(permissions & ~7u); }
SnxCtrMemResult snx_ctr_map(SnxCtrAddressSpace* s, uint32_t va, uint32_t pa, uint32_t size, unsigned permissions) {
    if (!s || !valid_range(va, size) || (pa & 4095) || !permissions_valid(permissions)) return SNX_CTR_MEM_INVALID;
    const SnxCtrRegion* region = snx_ctr_physical_region(s->memory->profile, pa, size);
    if (!region) return SNX_CTR_MEM_INVALID;
    if (region->kind == SNX_CTR_MMIO && (permissions & SNX_CTR_EXEC)) return SNX_CTR_MEM_PERMISSION;
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (p && p->present) return SNX_CTR_MEM_OVERLAP;
    }
    /* Allocating empty tables is harmless on failure; no PTE is published yet. */
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        unsigned index = (uint32_t)at >> 22;
        if (!s->tables[index]) s->tables[index] = calloc(1024, sizeof(Page));
        if (!s->tables[index]) return SNX_CTR_MEM_NOMEM;
    }
    for (uint64_t offset = 0; offset < size; offset += 4096)
        *page(s, va + (uint32_t)offset) = (Page){pa + (uint32_t)offset, (unsigned char)permissions, 1};
    changed(s->memory);
    return SNX_CTR_MEM_OK;
}
static SnxCtrMemResult change(SnxCtrAddressSpace* s, uint32_t va, uint32_t size, unsigned permissions, bool unmap) {
    if (!s || !valid_range(va, size) || !permissions_valid(permissions)) return SNX_CTR_MEM_INVALID;
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (!p || !p->present) return SNX_CTR_MEM_UNMAPPED;
        const SnxCtrRegion* r = snx_ctr_physical_region(s->memory->profile, p->pa, 4096);
        if (!unmap && (permissions & SNX_CTR_EXEC) && r->kind == SNX_CTR_MMIO) return SNX_CTR_MEM_PERMISSION;
    }
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (unmap)
            *p = (Page){0};
        else
            p->permissions = (unsigned char)permissions;
    }
    changed(s->memory);
    return SNX_CTR_MEM_OK;
}
SnxCtrMemResult snx_ctr_protect(SnxCtrAddressSpace* s, uint32_t va, uint32_t size, unsigned permissions) {
    return change(s, va, size, permissions, false);
}
SnxCtrMemResult snx_ctr_unmap(SnxCtrAddressSpace* s, uint32_t va, uint32_t size) {
    return change(s, va, size, 0, true);
}
SnxCtrMemResult snx_ctr_query(const SnxCtrAddressSpace* s, uint32_t va, uint32_t* pa, unsigned* permissions) {
    if (!s) return SNX_CTR_MEM_INVALID;
    Page* p = page(s, va);
    if (!p || !p->present) return SNX_CTR_MEM_UNMAPPED;
    if (pa) *pa = p->pa + (va & 4095);
    if (permissions) *permissions = p->permissions;
    return SNX_CTR_MEM_OK;
}
static unsigned char* physical(SnxCtrMemory* m, uint32_t pa, bool* mmio) {
    for (size_t i = 0; i < m->profile->region_count; ++i) {
        const SnxCtrRegion* r = &m->profile->regions[i];
        if (pa >= r->base && (uint64_t)pa < (uint64_t)r->base + r->size) {
            *mmio = r->kind == SNX_CTR_MMIO;
            return *mmio ? NULL : m->backing[i] + (pa - r->base);
        }
    }
    *mmio = true; /* only reachable for an internally corrupted PTE */
    return NULL;
}
static SnxCtrMemResult transfer(SnxCtrAddressSpace* s, uint32_t va, void* buffer, size_t size, unsigned access,
                                bool ram_only) {
    if (!s || !buffer || !size || size > UINT64_C(0x100000000) - va) return SNX_CTR_MEM_INVALID;
    size_t offset = 0;
    while (offset < size) {
        uint32_t at = va + (uint32_t)offset;
        Page* p = page(s, at);
        if (!p || !p->present) return SNX_CTR_MEM_UNMAPPED;
        if (!(p->permissions & access)) return SNX_CTR_MEM_PERMISSION;
        bool mmio;
        (void)physical(s->memory, p->pa, &mmio);
        size_t n = 4096 - (at & 4095);
        if (mmio) {
            if (ram_only || offset || size > n || (size != 1 && size != 2 && size != 4 && size != 8) ||
                (va & (size - 1)) || access == SNX_CTR_EXEC)
                return SNX_CTR_MEM_DEVICE;
            SnxCtrMemory* m = s->memory;
            uint32_t pa = p->pa + (at & 4095);
            bool ok = access == SNX_CTR_WRITE ? m->write && m->write(m->mmio_context, pa, buffer, size)
                                              : m->read && m->read(m->mmio_context, pa, buffer, size);
            if (ok && access == SNX_CTR_WRITE) changed(m);
            return ok ? SNX_CTR_MEM_OK : SNX_CTR_MEM_DEVICE;
        }
        if (n > size - offset) n = size - offset;
        offset += n;
    }
    for (offset = 0; offset < size;) {
        uint32_t at = va + (uint32_t)offset;
        Page* p = page(s, at);
        bool mmio;
        unsigned char* host = physical(s->memory, p->pa + (at & 4095), &mmio);
        size_t n = 4096 - (at & 4095);
        if (n > size - offset) n = size - offset;
        if (access == SNX_CTR_WRITE)
            memcpy(host, (const unsigned char*)buffer + offset, n);
        else
            memcpy((unsigned char*)buffer + offset, host, n);
        offset += n;
    }
    if (access == SNX_CTR_WRITE) changed(s->memory);
    return SNX_CTR_MEM_OK;
}
SnxCtrMemResult snx_ctr_read(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_READ, false);
}
SnxCtrMemResult snx_ctr_fetch(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_EXEC, false);
}
SnxCtrMemResult snx_ctr_write(SnxCtrAddressSpace* s, uint32_t va, const void* input, size_t size) {
    return transfer(s, va, (void*)input, size, SNX_CTR_WRITE, false);
}
SnxCtrMemResult snx_ctr_read_ram(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_READ, true);
}
SnxCtrMemResult snx_ctr_write_ram(SnxCtrAddressSpace* s, uint32_t va, const void* input, size_t size) {
    return transfer(s, va, (void*)input, size, SNX_CTR_WRITE, true);
}
