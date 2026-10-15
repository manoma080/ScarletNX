/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_AOT_RUNTIME_H
#define SNX_CTR_AOT_RUNTIME_H
#include <stddef.h>
/* Project-owned native scalar FP helpers, embedded at build time. No LLVM or
   guest code. Written only into the fresh private offline-link staging tree. */
extern const unsigned char snx_ctr_aot_runtime[];
extern const size_t snx_ctr_aot_runtime_size;
#endif
