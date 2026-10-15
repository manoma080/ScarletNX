/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/internal.h"
#include "ipc/ctr_ipc.h"
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__)
#include <sys/resource.h>
#endif
#define CMD (SNX_CTR_INITIAL_TLS + 0x80)
static void word(SnxCtrProcess* p, uint32_t address, uint32_t v) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (i * 8));
    assert(snx_ctr_write_ram(p->space, address, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, unsigned n) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, CMD + 4 * n, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void request(SnxCtrProcess* p, uint32_t h, const uint32_t* w, unsigned n, SnxCtrSvcResult state,
                    uint32_t transport) {
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, CMD, zero, 256) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < n; ++i) word(p, CMD + i * 4, w[i]);
    p->cpu.state.cpu.r[0] = h;
    assert(snx_ctr_svc(p, 0x32) == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? transport : h));
}
static bool no_mmio(void* context, uint32_t address, const void* bytes, size_t size) {
    (void)context;
    (void)address;
    (void)bytes;
    (void)size;
    assert(0 && "FS buffers must never invoke MMIO");
    return false;
}
static unsigned objects(SnxCtrProcess* p) {
    unsigned n = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) n += !!p->objects[i].type;
    return n;
}
static void archives(SnxCtrProcess* p, uint32_t fs, bool absent) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    unsigned before = objects(p);
    uint64_t serial = p->archive_sequence;
    a[5] = 0x304000;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
    assert(p->archive_sequence == serial && objects(p) == before);
    a[5] = 0x300020;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    if (absent) {
        assert(get(p, 1) == 0xc8804464 && !p->archive_sequence);
        return;
    }
    assert(get(p, 0) == 0x80c00c0 && !get(p, 1));
    uint64_t token = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
    assert(token && objects(p) == before);
    uint32_t open[] = {0x80201c2, 0, (uint32_t)token, (uint32_t)(token >> 32), 2, 12, 1, 0, 0x30002, 0x300000};
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x8020042 && !get(p, 1) && get(p, 2) == 0x10);
    uint32_t file = get(p, 3);
    uint32_t close[] = {0x80e0080, (uint32_t)token, (uint32_t)(token >> 32)};
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465);
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465);
    uint32_t size[] = {0x8040000};
    request(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get(p, 2));
    uint32_t finish[] = {0x8080000};
    request(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, file));
    uint64_t tokens[CTR_SELF_ARCHIVES];
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        tokens[i] = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
        assert(tokens[i] > token);
    }
    serial = p->archive_sequence;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xd0401834 && p->archive_sequence == serial && objects(p) == before);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        close[1] = (uint32_t)tokens[i];
        close[2] = (uint32_t)(tokens[i] >> 32);
        request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
    }
    p->archive_sequence = UINT32_MAX;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !get(p, 2) && get(p, 3) == 1);
    open[2] = 0;
    open[3] = 1;
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    file = get(p, 3);
    close[1] = 0;
    close[2] = 1;
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, file));
    serial = p->archive_sequence;
    p->archive_sequence = UINT64_MAX;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xd0401834 && p->archive_sequence == UINT64_MAX);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) assert(!p->self_archives[i].token);
    p->archive_sequence = serial;
}
int main(int argc, char** argv) {
    assert(argc == 4);
#if defined(__unix__)
    if (!strcmp(argv[3], "fallback")) {
        struct rlimit limit = {64, 64};
        assert(!setrlimit(RLIMIT_NOFILE, &limit));
    }
#endif
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 0x3000,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        bool absent = !strcmp(argv[3], "absent");
        assert(snx_ctr_process_mount_fs(p, argv[1]) && (!!p->romfs != absent));
        if (!absent) assert(!snx_ctr_process_mount_fs(p, argv[1]));
        uint32_t fs;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &fs));
        uint32_t open[] = {0x8030204, 0, 3, 1, 1, 2, 12, 1, 0, 0x4802, 0x300020, 0x30002, 0x300000};
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
        request(p, fs, init, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        unsigned before = objects(p);
        archives(p, fs, absent);
        if (absent) {
            request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(get(p, 1) == 0xc8804464 && objects(p) == before);
            snx_ctr_process_free(p);
            continue;
        }
        open[9] = 0x8802;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        assert(objects(p) == before && get(p, 0) == open[0]);
        open[9] = 0x4802;
        open[10] = 0x304000;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(objects(p) == before);
        open[10] = 0x300020;
        open[7] = 3;
        request(p, fs, open, 13, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(objects(p) == before);
        open[7] = 1;
        uint32_t aliases[CTR_HANDLES], n = 0, unused;
        while (n < CTR_HANDLES && !snx_ctr_handle_add(p, 1, &aliases[n])) ++n;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd8600413 && objects(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, aliases[i]));
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x8030042 && !get(p, 1) && get(p, 2) == 0x10);
        uint32_t file = get(p, 3), id = snx_ctr_handle_object(p, file), duplicate;
        assert(id && p->objects[id].type == CTR_OBJ_FILE && !snx_ctr_handle_add(p, id, &duplicate));
        uint32_t size_cmd[] = {0x8040000};
        request(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        uint64_t size = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
        assert(size && size < 12000);
        assert(get(p, 0) == 0x80400c0 && size == snx_ctr_romfs_size(p->romfs));
        unsigned char *expected = malloc((size_t)size), *actual = malloc((size_t)size);
        assert(expected && actual);
        assert(snx_ctr_romfs_read(p->romfs, 0, expected, (size_t)size));
        uint32_t read[] = {0x80200c2, 0, 0, (uint32_t)size, (uint32_t)size << 4 | 12, 0x300800};
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x8020082 && !get(p, 1) && get(p, 2) == size && get(p, 3) == read[4] &&
               get(p, 4) == read[5]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK &&
               !memcmp(actual, expected, (size_t)size));
        if (!model) assert(!snx_write_file(argv[2], actual, (size_t)size));
        for (uint32_t off = 0; off < size; off += 137) {
            size_t n = size - off < 139 ? (size_t)(size - off) : 139;
            assert(snx_ctr_romfs_read(p->romfs, off, actual, n) && !memcmp(actual, expected + off, n));
        }
        if (!strcmp(argv[3], "fallback")) {
            char* source = snx_path_join(argv[1], "romfs/hello.txt");
            char* moved = snx_path_join(argv[1], "romfs/hello.moved");
            assert(source && moved && !rename(source, moved));
            memset(actual, 0x55, (size_t)size);
            assert(snx_ctr_write_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            request(p, file, read, 6, SNX_CTR_SVC_UNSUPPORTED, 0);
            assert(get(p, 0) == read[0]);
            assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            for (uint64_t i = 0; i < size; ++i) assert(actual[i] == 0x55);
            assert(!rename(moved, source));
            free(source);
            free(moved);
        }
        read[1] = (uint32_t)size - 2;
        read[3] = 32;
        read[4] = 32 << 4 | 12;
        request(p, duplicate, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 2) == 2);
        read[1] = (uint32_t)size + 1;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 2));
        read[1] = read[2] = UINT32_MAX;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 2));
        read[1] = read[2] = 0;
        read[3] = 512;
        read[4] = 512 << 4 | 12;
        read[5] = 0x303f00;
        unsigned char fill[256];
        memset(fill, 0x55, sizeof fill);
        assert(snx_ctr_write_ram(p->space, read[5], fill, 256) == SNX_CTR_MEM_OK);
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(get(p, 0) == read[0]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, 256) == SNX_CTR_MEM_OK && !memcmp(fill, actual, 256));
        read[5] = 0x200000;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(snx_ctr_map(p->space, 0x50000000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
        snx_ctr_memory_mmio(p->memory, NULL, NULL, no_mmio);
        read[5] = 0x50000000;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        read[5] = 0x300800;
        read[4] = 16 << 4 | 12;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        read[4] = 512 << 4 | 10;
        request(p, file, read, 6, SNX_CTR_SVC_UNSUPPORTED, 0);
        uint32_t unknown[] = {0x8090000};
        request(p, file, unknown, 1, SNX_CTR_SVC_UNSUPPORTED, 0);
        uint32_t close[] = {0x8080000};
        request(p, file, close, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        request(p, duplicate, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        assert(!snx_ctr_handle_close(p, file) && p->objects[id].refs == 1 && !snx_ctr_handle_close(p, duplicate) &&
               !p->objects[id].type);
        request(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0xd8e007f7);
        for (n = 0; n < 64; ++n) {
            request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(!get(p, 1));
            aliases[n] = get(p, 3);
        }
        before = objects(p);
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd0401834 && objects(p) == before);
        for (n = 0; n < 64; ++n) assert(!snx_ctr_handle_close(p, aliases[n]));
        assert(!snx_ctr_handle_add(p, 1, &unused) && !snx_ctr_handle_close(p, unused));
        free(expected);
        free(actual);
        snx_ctr_process_free(p);
    }
    puts("CTR RomFS: real files, shared/fallback reads, checked buffers, quotas, close/duplicate lifetime passed");
    return 0;
}
