/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IPC_H
#define SNX_CTR_IPC_H
#include <stdint.h>
#define SNX_CTR_IPC_WORDS          64u
#define SNX_CTR_IPC_BAD_DESCRIPTOR 0xd9001830u
#define SNX_CTR_IPC_INVALID_STATE  0xe0a01bf5u
enum {
    SNX_CTR_SERVICE_NONE,
    SNX_CTR_SERVICE_SRV,
    SNX_CTR_SERVICE_FS,
    SNX_CTR_SERVICE_APT,
    SNX_CTR_SERVICE_PTM,
    SNX_CTR_SERVICE_HID
};
typedef enum { SNX_CTR_IPC_RETURN, SNX_CTR_IPC_UNSUPPORTED, SNX_CTR_IPC_BLOCKED, SNX_CTR_IPC_FAULT } SnxCtrIpcState;
struct SnxCtrProcess;
/* Serialized stack-owned request/reply copies. Never a flat guest pointer.
   Header: command[31:16], normal words[11:6], translation words[5:0].
   Result is reply normal word 1. PID descriptors are kernel supplied, not the
   raw client's claimed PID. Unknown commands never become successful stubs. */
typedef struct {
    struct SnxCtrProcess* process;
    uint32_t object, caller_pid, transport_result;
    uint32_t in[SNX_CTR_IPC_WORDS], out[SNX_CTR_IPC_WORDS];
    SnxCtrIpcState state;
} SnxCtrIpc;
int snx_ctr_ipc_expect(SnxCtrIpc* c, uint32_t normal, uint32_t translated);
void snx_ctr_ipc_bad(SnxCtrIpc* c);
void snx_ctr_ipc_get_bytes(SnxCtrIpc* c, uint32_t first, uint8_t* bytes, uint32_t size);
void snx_ctr_ipc_put_bytes(SnxCtrIpc* c, uint32_t first, const uint8_t* bytes, uint32_t size);
void snx_ctr_ipc_error_reply(SnxCtrIpc* c, uint32_t result);
/* Output handles are newly owned client-table references. Publication happens
   only after full TLS write preflight. Failed handlers must relinquish them. */
void snx_ctr_ipc_drop_handle(SnxCtrIpc* c, uint32_t handle);
void snx_ctr_ipc_output_handle(SnxCtrIpc* c, uint32_t word, uint32_t handle);
#endif
