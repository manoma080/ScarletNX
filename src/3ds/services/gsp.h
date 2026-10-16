/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_GSP_H
#define SNX_CTR_GSP_H
#include "ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
void snx_ctr_gsp_dispatch(SnxCtrIpc* c);
/* One host-paced vblank: shows latched framebuffers and queues PDC interrupts.
   Not cycle accurate. */
void snx_ctr_gsp_vblank(struct SnxCtrProcess* p);
bool snx_ctr_gsp_event(const struct SnxCtrProcess* p, uint32_t object);
uint32_t snx_ctr_gsp_wait(struct SnxCtrProcess* p, uint32_t object, uint32_t* registers);
#endif
