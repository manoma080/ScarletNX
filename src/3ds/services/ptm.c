/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/internal.h"
#include "services/ptm.swipc.inc"
static uint32_t snx_ctr_ptm_CheckNew3DS(SnxCtrIpc* c, uint32_t* is_new) {
    *is_new = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    return 0;
}
static uint32_t snx_ctr_ptm_ConfigureNew3DSCPU(SnxCtrIpc* c, uint32_t configuration) {
    /* The board implements only its base-rate ARM11 configuration, without
       accelerated L2/804-MHz modes. Requesting that existing configuration is
       idempotent; any request to enable an unimplemented mode stops. */
    if (configuration) c->state = SNX_CTR_IPC_UNSUPPORTED;
    return 0;
}
