/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MODULE_H
#define SNX_CTR_MODULE_H
#include "runtime/ctr_process.h"
#define SNX_CTR_MODULE_MAGIC         UINT64_C(0x31525443584e5353)
#define SNX_CTR_MODULE_ABI           1u
#define SNX_CTR_MODULE_CODEGEN_ABI   1u
#define SNX_CTR_MODULE_CHECKED_MODE  1u
/* Offline compilation bounds, NOT a general title-support guarantee. Tiny
   images retain exhaustive entries; larger images use conservative static CFG
   discovery. Uncovered indirect targets remain explicit unsupported exits. */
#define SNX_CTR_AOT_TEXT_LIMIT       (16u * 1024u * 1024u)
#define SNX_CTR_AOT_EXHAUSTIVE_LIMIT 4096u

typedef struct {
    uint64_t magic;
    uint32_t abi, descriptor_size, cpu_size, checked_size;
    uint32_t arm_abi, checked_abi, codegen_abi, profile, memory_mode, pointer_size;
    uint32_t text_address, text_bytes, arm_count, thumb_count;
    unsigned char image_identity[32];
    SnxArmBlock const* arm_blocks;
    SnxArmBlock const* thumb_blocks;
} SnxCtrModule;
/* ABI checks are compatibility checks, NOT a native-code sandbox. Only load
   freshly generated modules from trusted private host staging directories. */
bool snx_ctr_module_compatible(const SnxCtrModule* module, const SnxCtrImage* image);
typedef enum {
    SNX_CTR_RUN_EXIT,
    SNX_CTR_RUN_FAULT,
    SNX_CTR_RUN_UNSUPPORTED,
    SNX_CTR_RUN_BUDGET,
    SNX_CTR_RUN_BLOCKED
} SnxCtrRunResult;
/* Requires a compatible module and its matching freshly initialized process.
   Bounded static blocks; no discovery, compiler, interpreter or JIT here.
   Process-owned kernel HLE handles a bounded SVC subset. Unknown calls stop.
   Pending waits stop as BLOCKED and cannot be resumed past the pending call. */
SnxCtrRunResult snx_ctr_dispatch(const SnxCtrModule* module, SnxCtrProcess* process, uint64_t limit,
                                 uint64_t* executed);
typedef struct {
    void* library;
    const SnxCtrModule* module;
} SnxCtrNative;
/* Trusted native code only: dlopen may execute constructors before ABI checks.
   Never accept arbitrary host modules as though these checks were a sandbox. */
bool snx_ctr_native_open(const char* path, const SnxCtrImage* image, SnxCtrNative* native, char* error,
                         size_t error_size);
void snx_ctr_native_close(SnxCtrNative* native);
/* Native Linux pipeline: compile in a separate process, then load/run. New
   output only; no module cache/resume or loading arbitrary preexisting modules. */
int snx_ctr_aot_run(const char* driver, const char* directory, SnxCtrModel model, uint64_t limit, char* error,
                    size_t error_size);
#endif
