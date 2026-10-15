/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "aot/ctr_module.h"
#include "common.h"
#include "sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t read_word(SnxCtrAddressSpace* space, uint32_t va) {
    unsigned char b[4];
    assert(snx_ctr_read(space, va, b, sizeof b) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void snapshot(SnxCtrAddressSpace* space, const char* name, uint32_t va, uint32_t size) {
    unsigned char* bytes = malloc(size); unsigned char hash[32]; assert(bytes);
    assert(snx_ctr_read(space, va, bytes, size) == SNX_CTR_MEM_OK); snx_sha256(bytes, size, hash); free(bytes);
    printf("%s=", name); for (unsigned i = 0; i < 32; ++i) printf("%02x", hash[i]); puts("");
}
int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    char error[512];
    SnxCtrImage image;
    assert(snx_ctr_image_load(argv[1], &image, error, sizeof error));
    char* path = argc == 4 ? snx_strdup(argv[3]) : snx_format("%s/native/code.so", argv[1]);
    SnxCtrNative native;
    bool ok = snx_ctr_native_open(path, &image, &native, error, sizeof error);
    if (!strcmp(argv[2], "reject")) {
        assert(!ok && *error && !native.library && !native.module);
    } else {
        if (!ok) fprintf(stderr, "%s\n", error);
        assert(ok);
        SnxCtrProcess* process = snx_ctr_process_new(&image, SNX_CTR_OLD); assert(process);
        uint64_t executed;
        bool bootstrap = !strcmp(argv[2], "supertux_boot");
        SnxCtrRunResult state = snx_ctr_dispatch(native.module, process, bootstrap ? 100000 : 1000, &executed);
        SnxArmCPU* cpu = snx_ctr_process_cpu(process);
        if (bootstrap) {
            /* Pinned optional external fixture, not game-specific runtime code.
               The expected unsupported syscall is a BOOT gate, not gameplay. */
            assert(state == SNX_CTR_RUN_UNSUPPORTED && cpu->exit == SNX_ARM_SVC && cpu->syndrome == 0x21);
            assert(cpu->r[15] == 0x1deea8 && executed == 30949 && cpu->r[13] == SNX_CTR_STACK_TOP - 12);
            assert(cpu->r[0] == 0x2a4744 && cpu->r[14] == 0x1deca0 && !cpu->mem);
            assert(read_word(snx_ctr_process_space(process), 0x28d754) == 0);
            puts("SuperTux native bootstrap reached CreateAddressArbiter (unsupported): pc=0x001deea8 blocks=30949");
            for (unsigned reg = 0; reg < 16; ++reg) printf("r%u=%08x\n", reg, cpu->r[reg]);
            printf("cpsr=%08x\n", cpu->cpsr);
            snapshot(snx_ctr_process_space(process), "data", 0x28a000, 0x23000);
            snapshot(snx_ctr_process_space(process), "stack", SNX_CTR_STACK_TOP - image.stack_size, image.stack_size);
            snapshot(snx_ctr_process_space(process), "tls", SNX_CTR_INITIAL_TLS, 4096);
        } else if (!strcmp(argv[2], "workload")) {
            assert(state == SNX_CTR_RUN_EXIT);
            assert(executed == 17 && cpu->r[0] == 43 && !(cpu->cpsr & SNX_ARM_T));
            assert(cpu->r[13] == SNX_CTR_STACK_TOP && cpu->tpidruro == SNX_CTR_INITIAL_TLS);
            SnxCtrAddressSpace* space = snx_ctr_process_space(process);
            assert(read_word(space, 0x300000) == 42 && read_word(space, 0x301000) == 42);
            assert(read_word(space, SNX_CTR_INITIAL_TLS) == 42 && read_word(space, 0x200000) == 0x44414552);
        } else { assert(state == SNX_CTR_RUN_EXIT && !strcmp(argv[2], "basic") && executed == 2 && cpu->r[0] == 42); }
        snx_ctr_process_free(process);
    }
    snx_ctr_native_close(&native);
    snx_ctr_image_free(&image); free(path);
    puts("native loader/probe passed");
    return 0;
}
