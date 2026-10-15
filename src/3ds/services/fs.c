/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial FS session wire shapes referenced against Azahar service/fs/fs_user.cpp. */
#include "runtime/internal.h"
#include "fs.h"
#include "services/fs.swipc.inc"
static bool initialized(SnxCtrIpc* c) {
    return c->process->objects[c->object].data.session.client_pid == CTR_PROCESS_ID;
}
static uint32_t snx_ctr_fs_wire_InitializeWithSdkVersion(SnxCtrIpc* c, uint32_t version, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    CtrObject* object = &c->process->objects[c->object];
    object->data.session.client_pid = process;
    object->data.session.sdk_version = version;
    return 0;
}
static uint32_t snx_ctr_fs_wire_Initialize(SnxCtrIpc* c, uint32_t process) {
    return snx_ctr_fs_wire_InitializeWithSdkVersion(c, 0, process);
}
static CtrSelfArchive* archive(SnxCtrProcess* p, uint64_t token) {
    if (token)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i)
            if (p->self_archives[i].token == token) return &p->self_archives[i];
    return NULL;
}
static uint32_t snx_ctr_fs_wire_CloseArchive(SnxCtrIpc* c, uint64_t token) {
    if (!initialized(c)) return SNX_CTR_IPC_INVALID_STATE;
    CtrSelfArchive* a = archive(c->process, token);
    if (!a) return 0xc8804465u;
    a->token = 0;
    return 0;
}
/* An open file owns its process-backed view independently of archive tokens.
   The immutable RomFS owner outlives all file/archive clients. */
static void open_romfs(SnxCtrIpc* c, const unsigned char path[12]) {
    SnxCtrProcess* p = c->process;
    for (unsigned i = 0; i < 12; ++i)
        if (path[i]) {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
    if (!p->romfs) {
        snx_ctr_ipc_error_reply(c, 0xc8804464u);
        return;
    }
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) count += p->objects[i].type == CTR_OBJ_FILE;
    if (count >= 64) {
        snx_ctr_ipc_error_reply(c, 0xd0401834u);
        return;
    }
    uint32_t handle = 0, result = snx_ctr_object_create(p, CTR_OBJ_FILE, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x42;
    c->out[2] = 0x10;
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
static void open_file(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 7, 2)) return;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1] || c->in[4] != 2 || c->in[5] != 12 || c->in[6] != 1 || c->in[7]) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[8] != 0x30002) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char path[12];
    if (snx_ctr_read_ram(c->process->space, c->in[9], path, 12) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (!archive(c->process, (uint64_t)c->in[2] | (uint64_t)c->in[3] << 32)) {
        snx_ctr_ipc_error_reply(c, 0xc8804465u);
        return;
    }
    open_romfs(c, path);
}
static void open_direct(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 4)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    /* SelfNCCH, empty archive path, binary selector for this process' RomFS.
       Do not reinterpret Switch paths or pretend writable archives exist. */
    if (c->in[1] || c->in[2] != 3 || c->in[3] != 1 || c->in[4] != 1 || c->in[5] != 2 || c->in[6] != 12 ||
        c->in[7] != 1 || c->in[8]) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[9] != 0x4802 || c->in[11] != 0x30002) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char path[12], empty;
    if (snx_ctr_read_ram(p->space, c->in[10], &empty, 1) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, c->in[12], path, 12) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (empty) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    open_romfs(c, path);
}
static void open_archive(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    if (!initialized(c)) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if ((c->in[1] != 9 && c->in[1] != 3) || c->in[2] != 1 || c->in[3] != 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != 0x4002) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char empty;
    if (snx_ctr_read_ram(p->space, c->in[5], &empty, 1) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (empty) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* No SD backing exists. SelfNCCH is specifically a RomFS-backed, read-only
       binding, not general writable NCCH/ExeFS/SD access. */
    if (c->in[1] == 9 || !p->romfs) {
        snx_ctr_ipc_error_reply(c, c->in[1] == 9 ? 0xc8804478u : 0xc8804464u);
        return;
    }
    if (p->archive_sequence != UINT64_MAX)
        for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
            if (p->self_archives[i].token) continue;
            uint64_t token = ++p->archive_sequence;
            p->self_archives[i].token = token;
            c->out[0] = 0x080c00c0;
            c->out[2] = (uint32_t)token;
            c->out[3] = (uint32_t)(token >> 32);
            return;
        }
    snx_ctr_ipc_error_reply(c, 0xd0401834u);
}
void snx_ctr_fs_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x802: open_file(c); break;
        case 0x803: open_direct(c); break;
        case 0x80c: open_archive(c); break;
        default: snx_ctr_fs_wire_dispatch(c); break;
    }
}
