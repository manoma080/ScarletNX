/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial FS session wire shapes referenced against Azahar service/fs/fs_user.cpp. */
#include "runtime/internal.h"
#include "services/fs.swipc.inc"
static uint32_t snx_ctr_fs_InitializeWithSdkVersion(SnxCtrIpc* c, uint32_t version, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    CtrObject* object = &c->process->objects[c->object];
    object->data.session.client_pid = process;
    object->data.session.sdk_version = version;
    return 0;
}
static uint32_t snx_ctr_fs_Initialize(SnxCtrIpc* c, uint32_t process) {
    return snx_ctr_fs_InitializeWithSdkVersion(c, 0, process);
}
