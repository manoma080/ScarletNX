/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   SRV wire commands/results referenced against Azahar service/sm/{srv.cpp,sm.h}. */
#include "runtime/internal.h"
#include "services/srv.swipc.inc"
#include <string.h>
static uint32_t snx_ctr_srv_RegisterClient(SnxCtrIpc* c, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    c->process->objects[c->object].data.session.client_pid = process;
    return 0;
}
static uint32_t snx_ctr_srv_GetServiceHandle(SnxCtrIpc* c, const uint8_t* name, uint32_t length, uint32_t flags,
                                             uint32_t* session) {
    SnxCtrProcess* p = c->process;
    if (p->objects[c->object].data.session.client_pid != CTR_PROCESS_ID) return 0xd8e06406u;
    if (!length || length > 8) return 0xd9006405u;
    if (flags > 1) return 0xd8e093edu;
    for (unsigned i = 0; i < length; ++i) {
        if (!name[i]) return 0xd9006407u;
        if (name[i] < 0x21 || name[i] > 0x7e) return 0xd8e06406u;
    }
    if (length == 7 && !memcmp(name, "fs:USER", 7)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, session);
    if (flags & 1) return 0xd0406401u;
    /* This endpoint is genuinely absent. Waiting for its registration cannot
       complete in the current single-process/no-scheduler kernel. */
    p->waiting_object = c->object;
    memcpy(p->waiting_service, name, length);
    p->waiting_service[length] = 0;
    c->state = SNX_CTR_IPC_BLOCKED;
    return 0;
}
