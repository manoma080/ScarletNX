/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_KERNEL_H
#define SNX_CTR_KERNEL_H
#include "runtime/ctr_process.h"
typedef enum { SNX_CTR_SVC_RETURN, SNX_CTR_SVC_EXIT, SNX_CTR_SVC_UNSUPPORTED, SNX_CTR_SVC_BLOCKED } SnxCtrSvcResult;
/* Serialized user SVC register ABI. The instruction has already advanced PC.
   Unknown calls stop. A pending wait is not reported as successful completion. */
SnxCtrSvcResult snx_ctr_svc(SnxCtrProcess* process, uint32_t number);
bool snx_ctr_kernel_blocked(const SnxCtrProcess* process);
#endif
