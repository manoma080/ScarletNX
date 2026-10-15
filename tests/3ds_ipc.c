/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "aot/ctr_module.h"
#include "runtime/internal.h"
#include "ipc/ctr_ipc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND (SNX_CTR_INITIAL_TLS + 0x80)
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void request(SnxCtrProcess* p, const uint32_t* words, unsigned count) {
    unsigned char bytes[256] = {0};
    assert(count <= 64);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = (unsigned char)(words[i] >> (j * 8));
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
}
static void sync(SnxCtrProcess* p, uint32_t handle, SnxCtrSvcResult state, uint32_t result) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x32) == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? result : handle));
}
static uint32_t connect(SnxCtrProcess* p) {
    assert(snx_ctr_write_ram(p->space, 0x300100, "srv:", 5) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[1] = 0x300100;
    assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return p->cpu.state.cpu.r[1];
}
static void close_handle(SnxCtrProcess* p, uint32_t handle) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
}
static unsigned objects(SnxCtrProcess* p) {
    unsigned n = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) n += !!p->objects[i].type;
    return n;
}
static bool no_mmio(void* context, uint32_t address, void* bytes, size_t size) {
    (void)context;
    (void)address;
    (void)bytes;
    (void)size;
    assert(0 && "IPC must never invoke MMIO");
    return false;
}
static void lookup(SnxCtrProcess* p, uint32_t handle, uint32_t flags) {
    uint32_t args[] = {0x00050100, 0x553a7366, 0x00524553, 7, flags}; /* fs:USER */
    request(p, args, 5);
    sync(p, handle, SNX_CTR_SVC_RETURN, 0);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        uint32_t* r = p->cpu.state.cpu.r;
        r[1] = CTR_PROCESS_HANDLE;
        assert(snx_ctr_svc(p, 0x35) == SNX_CTR_SVC_RETURN && !r[0] && r[1] == CTR_PROCESS_ID);
        r[1] = CTR_THREAD_HANDLE;
        assert(snx_ctr_svc(p, 0x35) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007f7 && !r[1]);
        uint32_t srv = connect(p), id = snx_ctr_handle_object(p, srv);
        assert(id && p->objects[id].type == CTR_OBJ_SESSION && objects(p) == 3);
        lookup(p, srv, 1);
        assert(word(p, 0) == 0x00050040 && word(p, 1) == 0xd8e06406);
        uint32_t registration[] = {0x00010002, 0x20, 0xdeadbeef};
        request(p, registration, 3);
        /* Read-only/unmapped/misaligned/MMIO TLS: no handler mutation. */
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 1) == SNX_CTR_MEM_OK);
        sync(p, srv, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(!p->objects[id].data.session.client_pid && word(p, 0) == registration[0]);
        assert(snx_ctr_protect(p->space, SNX_CTR_INITIAL_TLS, 4096, 3) == SNX_CTR_MEM_OK);
        uint32_t tls_cases[] = {0x70000000, SNX_CTR_INITIAL_TLS + 1, 0xfffffe00, 0x50000000};
        assert(snx_ctr_map(p->space, 0x50000000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
        snx_ctr_memory_mmio(p->memory, NULL, no_mmio, NULL);
        for (unsigned i = 0; i < sizeof tls_cases / sizeof *tls_cases; ++i) {
            p->cpu.state.cpu.tpidruro = tls_cases[i];
            sync(p, srv, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
            assert(!p->objects[id].data.session.client_pid);
        }
        p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
        const unsigned counts[] = {0, 1, 62, 63};
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b) {
                uint32_t edge[64] = {0};
                edge[0] = 0x10000 | counts[a] << 6 | counts[b];
                if (counts[a] < 63) edge[1 + counts[a]] = 0x21;
                request(p, edge, 64);
                sync(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
                assert(!p->objects[id].data.session.client_pid && objects(p) == 3);
            }
        uint32_t bad_headers[] = {0x00010000, 0x00010040, 0x0001f002, 0x00010fff};
        for (unsigned i = 0; i < sizeof bad_headers / sizeof *bad_headers; ++i) {
            uint32_t bad[] = {bad_headers[i], 0x20, 0};
            request(p, bad, 3);
            sync(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
            assert(word(p, 0) == bad[0] && !p->objects[id].data.session.client_pid);
        }
        uint32_t malformed[] = {0x00010002, 0x21, 0};
        request(p, malformed, 3);
        sync(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        assert(!p->objects[id].data.session.client_pid);
        uint32_t truncated[] = {0x00010001, 0x20};
        request(p, truncated, 2);
        sync(p, srv, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        request(p, registration, 3);
        sync(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(word(p, 0) == 0x00010040 && !word(p, 1) && !word(p, 2));
        assert(p->objects[id].data.session.client_pid == CTR_PROCESS_ID); /* not forged PID */
        for (unsigned i = 2; i < 64; ++i) assert(!word(p, i));
        lookup(p, srv, 0);
        assert(word(p, 0) == 0x00050042 && !word(p, 1) && word(p, 2) == 0x10);
        uint32_t fs = word(p, 3), fid = snx_ctr_handle_object(p, fs);
        assert(fid && fid != id && p->objects[fid].data.session.service == SNX_CTR_SERVICE_FS);
        assert(objects(p) == 4 && !p->objects[fid].data.session.client_pid);
        uint32_t original_init[] = {0x08010002, 0x20, 0xdeadbeef};
        request(p, original_init, 3);
        sync(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(word(p, 0) == 0x08010040 && !word(p, 1) && p->objects[fid].data.session.client_pid == CTR_PROCESS_ID);
        uint32_t init[] = {0x08610042, 0x11223344, 0x20, 0xffffffff};
        request(p, init, 4);
        sync(p, fs, SNX_CTR_SVC_RETURN, 0);
        assert(word(p, 0) == 0x08610040 && !word(p, 1));
        assert(p->objects[fid].data.session.client_pid == CTR_PROCESS_ID &&
               p->objects[fid].data.session.sdk_version == 0x11223344);
        uint32_t unknown[] = {0xbeef0000};
        request(p, unknown, 1);
        sync(p, fs, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(word(p, 0) == unknown[0] && objects(p) == 4);
        /* Valid-but-unimplemented CTR mapped buffers stop, not HIPC parsing. */
        uint32_t buffer[] = {0x08010002, 0x0000100a, 0x300000};
        request(p, buffer, 3);
        sync(p, fs, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(word(p, 0) == buffer[0]);
        close_handle(p, fs);
        request(p, init, 4);
        sync(p, fs, SNX_CTR_SVC_RETURN, 0xd8e007f7);
        assert(word(p, 0) == init[0] && objects(p) == 3);
        uint32_t ports[63];
        for (unsigned i = 0; i < 63; ++i) ports[i] = connect(p);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xd0401834 && !r[1]);
        for (unsigned i = 0; i < 63; ++i) close_handle(p, ports[i]);
        const uint32_t bad_names[] = {0xffffffff, 0x70000000, 0x50000000};
        for (unsigned i = 0; i < 3; ++i) {
            r[1] = bad_names[i];
            assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e01bf5 && !r[1] && objects(p) == 3);
        }
        assert(snx_ctr_write_ram(p->space, 0x300100, "other", 6) == SNX_CTR_MEM_OK);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xd88007fa && !r[1]);
        assert(snx_ctr_write_ram(p->space, 0x300100, "abcdefghijkl", 12) == SNX_CTR_MEM_OK);
        r[1] = 0x300100;
        assert(snx_ctr_svc(p, 0x2d) == SNX_CTR_SVC_RETURN && r[0] == 0xe0e0181e && !r[1]);
        /* A duplicated session shares client identity and owns a reference. */
        r[1] = srv;
        assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t duplicate = r[1];
        close_handle(p, srv);
        lookup(p, duplicate, 0);
        assert(!word(p, 1) && snx_ctr_handle_object(p, word(p, 3)));
        close_handle(p, word(p, 3));
        srv = duplicate;
        /* Full handle table: tentative session object is rolled back. */
        uint32_t extras[CTR_HANDLES];
        unsigned count = 0;
        for (;;) {
            r[1] = CTR_PROCESS_HANDLE;
            assert(snx_ctr_svc(p, 0x27) == SNX_CTR_SVC_RETURN);
            if (r[0]) break;
            extras[count++] = r[1];
        }
        assert(count == CTR_HANDLES - 1 && objects(p) == 3);
        lookup(p, srv, 0);
        assert(word(p, 0) == 0x00050040 && word(p, 1) == 0xd8600413 && objects(p) == 3);
        for (unsigned i = 0; i < count; ++i) close_handle(p, extras[i]);
        uint32_t sessions[32];
        for (unsigned i = 0; i < 32; ++i) {
            lookup(p, srv, 0);
            assert(!word(p, 1));
            sessions[i] = word(p, 3);
        }
        lookup(p, srv, 0);
        assert(word(p, 1) == 0xd0401834 && objects(p) == 35);
        for (unsigned i = 0; i < 32; ++i) close_handle(p, sessions[i]);
        uint32_t absent[] = {0x00050100, 0x5353494d, 0x474e49, 7, 1}; /* MISSING, non-waiting */
        request(p, absent, 5);
        sync(p, srv, SNX_CTR_SVC_RETURN, 0);
        assert(word(p, 0) == 0x00050040 && word(p, 1) == 0xd0406401 && !snx_ctr_kernel_blocked(p));
        absent[4] = 0;
        request(p, absent, 5);
        sync(p, srv, SNX_CTR_SVC_BLOCKED, 0);
        assert(word(p, 0) == absent[0] && !strcmp(snx_ctr_thread(p)->waiting_service, "MISSING") &&
               snx_ctr_kernel_blocked(p));
        uint64_t executed = 99;
        SnxCtrModule empty = {0};
        assert(snx_ctr_dispatch(&empty, p, 1, &executed) == SNX_CTR_RUN_BLOCKED && !executed);
        assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_BLOCKED && objects(p) == 3);
        snx_ctr_process_free(p);
    }
    puts("CTR IPC/session identity, PID replacement, permissions, descriptors, quotas and truthful waits passed");
    return 0;
}
