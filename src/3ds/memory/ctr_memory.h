/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MEMORY_H
#define SNX_CTR_MEMORY_H
#include "board/board.h"
#define SNX_CTR_PAGE_SIZE 4096u
#define SNX_CTR_READ      1u
#define SNX_CTR_WRITE     2u
#define SNX_CTR_EXEC      4u

typedef enum {
    SNX_CTR_MEM_OK,
    SNX_CTR_MEM_INVALID,
    SNX_CTR_MEM_NOMEM,
    SNX_CTR_MEM_UNMAPPED,
    SNX_CTR_MEM_PERMISSION,
    SNX_CTR_MEM_OVERLAP,
    SNX_CTR_MEM_DEVICE
} SnxCtrMemResult;
typedef struct SnxCtrMemory SnxCtrMemory;
typedef struct SnxCtrAddressSpace SnxCtrAddressSpace;
/* MMIO calls are a single aligned 1/2/4/8-byte access in little-endian byte
   order. False means a device fault. Execute access to MMIO is always denied. */
typedef bool (*SnxCtrMmioRead)(void* context, uint32_t physical, void* bytes, size_t size);
typedef bool (*SnxCtrMmioWrite)(void* context, uint32_t physical, const void* bytes, size_t size);
SnxCtrMemory* snx_ctr_memory_new(SnxCtrModel model);
/* Address spaces must be destroyed first. No internal locking: caller serializes
   mappings/accesses/devices. Physical RAM is shared by all spaces on this board. */
void snx_ctr_memory_free(SnxCtrMemory* memory);
void snx_ctr_memory_mmio(SnxCtrMemory* memory, void* context, SnxCtrMmioRead read, SnxCtrMmioWrite write);
SnxCtrAddressSpace* snx_ctr_space_new(SnxCtrMemory* memory);
void snx_ctr_space_free(SnxCtrAddressSpace* space);
/* Page-aligned nonempty ranges; mapping requires one physical board region.
   Failed map/protect/unmap leave all mappings unchanged. No automatic user VA
   layout is installed: process/HLE setup owns the allowed address ranges. */
SnxCtrMemResult snx_ctr_map(SnxCtrAddressSpace* space, uint32_t va, uint32_t pa, uint32_t size, unsigned permissions);
SnxCtrMemResult snx_ctr_protect(SnxCtrAddressSpace* space, uint32_t va, uint32_t size, unsigned permissions);
SnxCtrMemResult snx_ctr_unmap(SnxCtrAddressSpace* space, uint32_t va, uint32_t size);
SnxCtrMemResult snx_ctr_query(const SnxCtrAddressSpace* space, uint32_t va, uint32_t* pa, unsigned* permissions);
/* Serialized board-wide reservation generation, not a RAM value comparison.
   Every successful write or mapping/protection change advances it. Saturates
   at UINT64_MAX, after which exclusive stores must fail conservatively. */
uint64_t snx_ctr_memory_epoch(const SnxCtrAddressSpace* space);
/* RAM transfers may be unaligned and span pages, after preflighting the ENTIRE
   range. MMIO transfers may not span pages or mix with RAM. No flat host pointer
   is exported. The CPU adapter owns instruction-specific alignment rules. */
SnxCtrMemResult snx_ctr_read(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
SnxCtrMemResult snx_ctr_write(SnxCtrAddressSpace* space, uint32_t va, const void* input, size_t size);
SnxCtrMemResult snx_ctr_fetch(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
/* Whole-instruction multiword transfers: reject any MMIO before side effects,
   rather than collapsing LDM/STM or paired accesses into one device access. */
SnxCtrMemResult snx_ctr_read_ram(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
SnxCtrMemResult snx_ctr_write_ram(SnxCtrAddressSpace* space, uint32_t va, const void* input, size_t size);
#endif
