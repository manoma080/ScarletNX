/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PROCESS_H
#define SNX_CTR_PROCESS_H
#include "cpu/ctr_cpu.h"
#include "loader/ctr_image.h"
#define SNX_CTR_STACK_TOP   0x10000000u
#define SNX_CTR_INITIAL_TLS 0x1ff82000u

typedef struct SnxCtrProcess SnxCtrProcess;
/* Single fresh USR thread. Image must come from snx_ctr_image_load. Owns a
   bounded handle table, kernel objects, private read-only configuration page
   and checked heap/linear allocations. Initial checked CTR IPC/session support;
   no scheduler, APT, FS resources or devices yet.
   BSS follows page-padded data, matching extraction and Azahar loader policy. */
SnxCtrProcess* snx_ctr_process_new(const SnxCtrImage* image, SnxCtrModel model);
void snx_ctr_process_free(SnxCtrProcess* process);
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* process);
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* process);
#endif
