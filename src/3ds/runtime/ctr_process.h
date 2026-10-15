/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PROCESS_H
#define SNX_CTR_PROCESS_H
#include "cpu/ctr_cpu.h"
#include "loader/ctr_image.h"
#define SNX_CTR_STACK_TOP   0x10000000u
#define SNX_CTR_INITIAL_TLS 0x1ff82000u

typedef struct SnxCtrProcess SnxCtrProcess;
/* Fresh USR process. Image must come from snx_ctr_image_load. Owns bounded
   native thread contexts, typed/refcounted objects and handles, read-only
   configuration, checked heap/linear mappings and minimal startup services.
   BSS follows page-padded data, matching extraction and Azahar loader policy. */
SnxCtrProcess* snx_ctr_process_new(const SnxCtrImage* image, SnxCtrModel model);
/* Once, before execution. Same indexed/shared host files as NX, exposed with
   CTR metadata and IPC. Requires a fresh trusted, unmodified extracted tree. */
bool snx_ctr_process_mount_fs(SnxCtrProcess* process, const char* directory);
void snx_ctr_process_free(SnxCtrProcess* process);
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* process);
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* process);
#endif
