/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_module.h"
#include "kernel/ctr_kernel.h"
#include "runtime/internal.h"
#include <string.h>

bool snx_ctr_module_compatible(const SnxCtrModule* m, const SnxCtrImage* image) {
    return m && image && m->magic == SNX_CTR_MODULE_MAGIC && m->abi == SNX_CTR_MODULE_ABI &&
           m->descriptor_size == sizeof *m && m->cpu_size == sizeof(SnxArmCPU) &&
           m->checked_size == sizeof(SnxArmCheckedCPU) && m->arm_abi == SNX_ARM_ABI &&
           m->checked_abi == SNX_ARM_CHECKED_ABI && m->codegen_abi == SNX_CTR_MODULE_CODEGEN_ABI &&
           m->profile == SNX_ARM_PROFILE_ARM11 && m->memory_mode == SNX_CTR_MODULE_CHECKED_MODE &&
           m->pointer_size == sizeof(void*) && m->text_address == image->segment[0].address &&
           m->text_bytes == image->segment[0].bytes && m->text_bytes && !(m->text_bytes & 3) &&
           m->text_bytes <= SNX_CTR_AOT_TEXT_LIMIT && m->arm_count == m->text_bytes / 4 &&
           m->thumb_count == m->text_bytes / 2 && m->arm_blocks && m->thumb_blocks &&
           !memcmp(m->image_identity, image->identity, 32);
}
SnxCtrRunResult snx_ctr_dispatch(const SnxCtrModule* m, SnxCtrProcess* process, uint64_t limit, uint64_t* executed) {
    if (!executed) return SNX_CTR_RUN_FAULT;
    *executed = 0;
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (!cpu || !m) return SNX_CTR_RUN_FAULT;
    if (!snx_ctr_schedule(process)) return SNX_CTR_RUN_BLOCKED;
    while (*executed < limit) {
        uint32_t pc = cpu->r[15];
        bool thumb = !!(cpu->cpsr & SNX_ARM_T);
        unsigned width = thumb ? 2 : 4;
        if (pc & (width - 1)) {
            cpu->exit = SNX_ARM_ALIGNMENT;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_FAULT;
        }
        unsigned char instruction[4];
        if (snx_ctr_fetch(snx_ctr_process_space(process), pc, instruction, width) != SNX_CTR_MEM_OK) {
            cpu->exit = SNX_ARM_PREFETCH_ABORT;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_FAULT;
        }
        if (pc < m->text_address || (uint64_t)pc + width > (uint64_t)m->text_address + m->text_bytes) {
            cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_UNSUPPORTED;
        }
        uint32_t index = (pc - m->text_address) / width;
        SnxArmBlock block = thumb ? m->thumb_blocks[index] : m->arm_blocks[index];
        if (!block) {
            cpu->exit = SNX_ARM_BACKEND_UNSUPPORTED;
            cpu->syndrome = pc;
            return SNX_CTR_RUN_UNSUPPORTED;
        }
        block(cpu);
        ++*executed;
        if (cpu->exit == SNX_ARM_OK) continue;
        if (cpu->exit == SNX_ARM_SVC) {
            SnxCtrSvcResult result = snx_ctr_svc(process, cpu->syndrome);
            if (result == SNX_CTR_SVC_EXIT) return SNX_CTR_RUN_EXIT;
            if (result == SNX_CTR_SVC_BLOCKED) {
                if (!snx_ctr_schedule(process)) return SNX_CTR_RUN_BLOCKED;
                continue;
            }
            if (result == SNX_CTR_SVC_UNSUPPORTED) return SNX_CTR_RUN_UNSUPPORTED;
            cpu->exit = SNX_ARM_OK;
            cpu->syndrome = 0;
            if (!snx_ctr_schedule(process)) return SNX_CTR_RUN_BLOCKED;
            continue;
        }
        if (cpu->exit == SNX_ARM_BACKEND_UNSUPPORTED || cpu->exit == SNX_ARM_CODE_CHANGED)
            return SNX_CTR_RUN_UNSUPPORTED;
        return SNX_CTR_RUN_FAULT;
    }
    return SNX_CTR_RUN_BUDGET;
}
