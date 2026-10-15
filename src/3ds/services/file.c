/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR FSFILE wire facts: libctru services/fs.c and Azahar service/fs/file.cpp.
   Read-only file sessions over the process' shared, indexed host RomFS view. */
#include "runtime/internal.h"
#include "file.h"
#include "services/file.swipc.inc"
#include <stdlib.h>
static uint32_t snx_ctr_file_wire_GetSize(SnxCtrIpc* c, uint64_t* size) {
    if (c->process->objects[c->object].data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    *size = snx_ctr_romfs_size(c->process->romfs);
    return 0;
}
static uint32_t snx_ctr_file_wire_Close(SnxCtrIpc* c) {
    if (c->process->objects[c->object].data.file.closed) return SNX_CTR_IPC_INVALID_STATE;
    c->process->objects[c->object].data.file.closed = 1;
    return 0;
}
void snx_ctr_file_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (c->in[0] >> 16 != 0x802) {
        snx_ctr_file_wire_dispatch(c);
        return;
    }
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    if (p->objects[c->object].data.file.closed) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    uint32_t size = c->in[3], address = c->in[5];
    if (size > 16u * 1024 * 1024) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != (size << 4 | 12)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (snx_ctr_check_ram(p->space, address, size, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint64_t offset = (uint64_t)c->in[1] | (uint64_t)c->in[2] << 32, total = snx_ctr_romfs_size(p->romfs);
    uint32_t n = offset >= total ? 0 : total - offset < size ? (uint32_t)(total - offset) : size;
    unsigned char* data = malloc(n ? n : 1);
    if (!data) {
        snx_ctr_ipc_error_reply(c, 0xd86007f3u);
        return;
    }
    /* Stage the complete result before touching guest RAM: on-demand host IO
       failure, bad spans, MMIO and permissions cannot publish a partial read. */
    if (n && !snx_ctr_romfs_read(p->romfs, offset, data, n)) {
        free(data);
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    if (n && snx_ctr_write_ram(p->space, address, data, n) != SNX_CTR_MEM_OK) {
        free(data);
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    free(data);
    c->out[0] = 0x08020082;
    c->out[2] = n;
    c->out[3] = c->in[4];
    c->out[4] = address;
}
