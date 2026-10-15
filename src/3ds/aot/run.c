/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_module.h"
#include "common.h"
#include "runtime/internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int snx_ctr_aot_run(const char* driver, const char* directory, SnxCtrModel model, uint64_t limit, char* error,
                    size_t error_size) {
    SnxCtrImage image = {0};
    SnxCtrProcess* process = NULL;
    SnxCtrNative native = {0};
    char *executable = NULL, *parent = NULL, *compiler = NULL, *module_path = NULL;
    int result = 1;
    if (!snx_ctr_image_load(directory, &image, error, error_size)) goto done;
    executable = snx_executable(driver);
    if (!executable) {
        snprintf(error, error_size, "cannot locate 3DS compiler beside driver");
        goto done;
    }
    parent = snx_dirname(executable);
    compiler = snx_path_join(parent, "scarletnx-3ds-aot");
    if (!snx_is_file(compiler)) {
        snprintf(error, error_size, "missing adjacent scarletnx-3ds-aot compiler");
        result = 3;
        goto done;
    }
    printf("Recompiling ARM11 (offline static AOT)\n");
    const char* command[] = {compiler, directory, NULL};
    int status = snx_run_stage(command, NULL, "ARM11 static compilation");
    if (status) {
        snprintf(error, error_size, "ARM11 static compiler failed (status %d); guest was not run", status);
        result = status == 3 ? 3 : 1;
        goto done;
    }
    module_path = snx_format("%s/native/code.so", directory);
    if (!snx_ctr_native_open(module_path, &image, &native, error, error_size)) goto done;
    process = snx_ctr_process_new(&image, model);
    if (!process) {
        snprintf(error, error_size, "cannot create checked ARM11 process mappings");
        goto done;
    }
    if (!snx_ctr_process_mount_fs(process, directory)) {
        snprintf(error, error_size, "cannot index extracted title filesystem");
        goto done;
    }
    uint64_t executed;
    printf("Running ARM11 static AOT\n");
    SnxCtrRunResult state = snx_ctr_dispatch(native.module, process, limit, &executed);
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (state == SNX_CTR_RUN_EXIT) {
        printf("Guest ExitProcess: r0=%" PRIu32 " blocks=%" PRIu64 "\n", cpu->r[0], executed);
        result = 0;
    } else {
        snprintf(error, error_size,
                 "%s: pc=0x%08" PRIx32 " exit=%u syndrome=0x%08" PRIx32 " blocks=%" PRIu64 " svcs=%" PRIu64
                 " last_svc=0x%02" PRIx32 " ipc_service=%" PRIu32 " ipc_header=0x%08" PRIx32 " wait_service=%s",
                 state == SNX_CTR_RUN_BLOCKED       ? "guest blocked (no runnable thread)"
                 : state == SNX_CTR_RUN_BUDGET      ? "static block budget exhausted"
                 : state == SNX_CTR_RUN_UNSUPPORTED ? "unsupported guest operation or missing static entry"
                                                    : "guest fault",
                 cpu->r[15], cpu->exit, cpu->syndrome, executed, process->svc_count, process->last_svc,
                 process->last_ipc_service, process->last_ipc_header, snx_ctr_thread(process)->waiting_service);
        result = state == SNX_CTR_RUN_FAULT ? 1 : 3;
    }
done:
    snx_ctr_process_free(process);
    snx_ctr_native_close(&native);
    free(module_path);
    free(compiler);
    free(parent);
    free(executable);
    snx_ctr_image_free(&image);
    return result;
}
