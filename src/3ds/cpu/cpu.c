/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_cpu.h"
#include <string.h>

static uint32_t access_memory(void* context, uint32_t op, uint32_t va, uint32_t size, void* bytes) {
    SnxCtrCpu* cpu = context;
    if (!cpu || !size || size > 64 || !bytes) return 1;
    SnxCtrAddressSpace* space = cpu->space;
    SnxCtrMemResult result;
    if (op == (SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_EXCLUSIVE) ||
        op == (SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_EXCLUSIVE)) {
        if ((size != 1 && size != 2 && size != 4 && size != 8) || (va & (size - 1))) return 1;
        if (op & SNX_ARM_ACCESS_READ) {
            cpu->reservation_epoch = 0;
            if (snx_ctr_read_ram(space, va, bytes, size) != SNX_CTR_MEM_OK) return 1;
            cpu->reservation_address = va;
            cpu->reservation_size = size;
            cpu->reservation_epoch = snx_ctr_memory_epoch(space);
            return 0;
        }
        uint64_t epoch = cpu->reservation_epoch;
        cpu->reservation_epoch = 0;
        if (!epoch || epoch == UINT64_MAX || epoch != snx_ctr_memory_epoch(space) || cpu->reservation_address != va ||
            cpu->reservation_size != size)
            return SNX_ARM_EXCLUSIVE_FAILED;
        return snx_ctr_write_ram(space, va, bytes, size) == SNX_CTR_MEM_OK ? 0 : 1;
    }
    switch (op) {
        case SNX_ARM_ACCESS_READ: result = snx_ctr_read(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE: result = snx_ctr_write(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_FETCH: result = snx_ctr_fetch(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_MULTI: result = snx_ctr_read_ram(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_MULTI: result = snx_ctr_write_ram(space, va, bytes, size); break;
        default: return 1;
    }
    return result == SNX_CTR_MEM_OK ? 0 : 1;
}
bool snx_ctr_cpu_init(SnxCtrCpu* cpu, SnxCtrAddressSpace* space, uint32_t pc, uint32_t sp, bool thumb) {
    if (!cpu || !space || (pc & (thumb ? 1u : 3u)) || (sp & 7)) return false;
    memset(cpu, 0, sizeof *cpu);
    cpu->state.cpu.cpsr = 0x10u | (thumb ? SNX_ARM_T : 0);
    cpu->state.cpu.r[15] = pc;
    cpu->state.cpu.r[13] = sp;
    cpu->state.abi = SNX_ARM_CHECKED_ABI;
    cpu->space = space;
    cpu->state.context = cpu;
    cpu->state.access = access_memory;
    return true;
}
