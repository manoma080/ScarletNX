/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial FS session wire shapes referenced against Azahar service/fs/fs_user.cpp. */
#include "runtime/internal.h"
#include "fs.h"
#include "services/fs.swipc.inc"
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
void snx_ctr_fs_dispatch(SnxCtrIpc* c) {
    if (c->in[0] >> 16 != 0x80c) {
        snx_ctr_fs_wire_dispatch(c);
        return;
    }
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    if (c->process->objects[c->object].data.session.client_pid != CTR_PROCESS_ID) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    if (c->in[1] != 9 || c->in[2] != 1 || c->in[3] != 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != 0x4002) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char empty;
    if (snx_ctr_read_ram(c->process->space, c->in[5], &empty, 1) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (empty) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* No SD device/backing has been configured. SDMC archive 9 is genuinely
       absent (FS NotFound), not a fabricated successful mount/archive handle.
       Other archives and all file operations remain explicitly unsupported. */
    snx_ctr_ipc_error_reply(c, 0xc8804478u);
}
