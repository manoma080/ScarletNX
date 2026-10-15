/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR header/PID/handle descriptor facts referenced against Citra/Azahar
   ipc.h and kernel/hle_ipc.cpp, GPL-2.0-or-later, revision in docs/3DS.md. */
#include "ctr_ipc.h"
#include "runtime/internal.h"
#include "services/srv.swipc.h"
#include "services/fs.swipc.h"
#include <string.h>

static uint32_t load32(const unsigned char* b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void store32(unsigned char* b, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (i * 8));
}
void snx_ctr_ipc_bad(SnxCtrIpc* c) { c->transport_result = SNX_CTR_IPC_BAD_DESCRIPTOR; }
int snx_ctr_ipc_expect(SnxCtrIpc* c, uint32_t normal, uint32_t translated) {
    if (((c->in[0] >> 6) & 63) != normal || (c->in[0] & 63) != translated) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    return 1;
}
void snx_ctr_ipc_get_bytes(SnxCtrIpc* c, uint32_t first, uint8_t* bytes, uint32_t size) {
    if (first >= SNX_CTR_IPC_WORDS || size > (SNX_CTR_IPC_WORDS - first) * 4) {
        snx_ctr_ipc_bad(c);
        return;
    }
    for (uint32_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(c->in[first + i / 4] >> ((i & 3) * 8));
}
void snx_ctr_ipc_put_bytes(SnxCtrIpc* c, uint32_t first, const uint8_t* bytes, uint32_t size) {
    if (first >= SNX_CTR_IPC_WORDS || size > (SNX_CTR_IPC_WORDS - first) * 4) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    for (uint32_t i = 0; i < size; ++i) c->out[first + i / 4] |= (uint32_t)bytes[i] << ((i & 3) * 8);
}
void snx_ctr_ipc_error_reply(SnxCtrIpc* c, uint32_t result) {
    if (c->transport_result || c->state != SNX_CTR_IPC_RETURN) return;
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
    c->out[1] = result;
}
void snx_ctr_ipc_drop_handle(SnxCtrIpc* c, uint32_t handle) {
    if (handle) (void)snx_ctr_handle_close(c->process, handle);
}
void snx_ctr_ipc_output_handle(SnxCtrIpc* c, uint32_t first, uint32_t descriptor, uint32_t handle) {
    if (first < 2 || first >= SNX_CTR_IPC_WORDS - 1 || (descriptor != 0 && descriptor != 0x10) || !handle ||
        handle == CTR_PROCESS_HANDLE || handle == CTR_THREAD_HANDLE || !snx_ctr_handle_object(c->process, handle)) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[first] = descriptor;
    c->out[first + 1] = handle;
}
uint32_t snx_ctr_session_create(SnxCtrProcess* p, uint32_t service, uint32_t* out) {
    *out = 0;
    if (service != SNX_CTR_SERVICE_SRV && service != SNX_CTR_SERVICE_FS) return 0xe0e01bf4u;
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_SESSION && p->objects[i].data.session.service == service) ++count;
    if (count >= (service == SNX_CTR_SERVICE_SRV ? 64u : 32u)) return 0xd0401834u;
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_SESSION, out);
    if (!result) p->objects[snx_ctr_handle_object(p, *out)].data.session.service = service;
    return result;
}
uint32_t snx_ctr_named_connect(SnxCtrProcess* p, uint32_t address, uint32_t* out) {
    char name[12];
    *out = 0;
    for (unsigned i = 0; i < sizeof name; ++i) {
        if ((uint64_t)address + i > UINT32_MAX ||
            snx_ctr_read_ram(p->space, address + i, &name[i], 1) != SNX_CTR_MEM_OK)
            return 0xe0e01bf5u;
        if (!name[i]) return !strcmp(name, "srv:") ? snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, out) : 0xd88007fau;
    }
    return 0xe0e0181eu;
}
enum { WIRE_OK, WIRE_BAD, WIRE_UNSUPPORTED };
static unsigned translation(const SnxCtrIpc* c, uint32_t normal, uint32_t translated) {
    uint32_t position = 1 + normal, end = position + translated;
    bool unsupported = false;
    while (position < end) {
        uint32_t d = c->in[position++], payload;
        /* Buffer forms must be classified before copy/move handle forms. */
        if (d & 8) {
            if ((d & 15) != 10 && (d & 15) != 12 && (d & 15) != 14) return WIRE_BAD;
            payload = 1;
            unsupported = true;
        } else if ((d & 1023) == 2 || (d & 15) == 4 || (d & 15) == 6) {
            payload = 1;
            unsupported = true;
        } else if (d == 0x20) {
            payload = 1;
        } else if (!(d & 0x03ffffffu) || (d & 0x03ffffffu) == 0x10) {
            payload = (d >> 26) + 1;
            unsupported = true;
        } else
            return WIRE_BAD;
        if (payload > end - position) return WIRE_BAD;
        position += payload;
    }
    return unsupported ? WIRE_UNSUPPORTED : WIRE_OK;
}
SnxCtrSvcResult snx_ctr_ipc_send(SnxCtrProcess* p, uint32_t handle, uint32_t* result) {
    *result = 0;
    uint32_t id = snx_ctr_handle_object(p, handle);
    if (!id || p->objects[id].type != CTR_OBJ_SESSION) {
        *result = 0xd8e007f7u;
        return SNX_CTR_SVC_RETURN;
    }
    uint32_t tls = p->cpu.state.cpu.tpidruro;
    uint32_t address = tls + 0x80;
    unsigned permissions;
    unsigned char bytes[SNX_CTR_IPC_WORDS * 4];
    /* A 0x200-aligned TLS slot contains the entire 0x100-byte command buffer.
       Preflight RAM read AND write before a handler can change state/handles.
       All mappings, handlers and writes are serialized by the runtime. */
    if ((tls & 511) || (uint64_t)tls + 0x180 > UINT64_C(0x100000000) ||
        snx_ctr_query(p->space, address, NULL, &permissions) != SNX_CTR_MEM_OK || (permissions & 3) != 3 ||
        snx_ctr_read_ram(p->space, address, bytes, sizeof bytes) != SNX_CTR_MEM_OK) {
        *result = 0xe0e01bf5u;
        return SNX_CTR_SVC_RETURN;
    }
    SnxCtrIpc c = {.process = p, .object = id, .caller_pid = CTR_PROCESS_ID};
    for (unsigned i = 0; i < SNX_CTR_IPC_WORDS; ++i) c.in[i] = load32(bytes + i * 4);
    p->last_ipc_header = c.in[0];
    p->last_ipc_service = p->objects[id].data.session.service;
    uint32_t normal = (c.in[0] >> 6) & 63, translated = c.in[0] & 63;
    if ((c.in[0] & 0xf000) || 1 + normal + translated > SNX_CTR_IPC_WORDS) {
        *result = SNX_CTR_IPC_BAD_DESCRIPTOR;
        return SNX_CTR_SVC_RETURN;
    }
    unsigned wire = translation(&c, normal, translated);
    if (wire == WIRE_UNSUPPORTED) return SNX_CTR_SVC_UNSUPPORTED;
    if (wire == WIRE_BAD) {
        *result = SNX_CTR_IPC_BAD_DESCRIPTOR;
        return SNX_CTR_SVC_RETURN;
    }
    switch (p->last_ipc_service) {
        case SNX_CTR_SERVICE_SRV: snx_ctr_srv_dispatch(&c); break;
        case SNX_CTR_SERVICE_FS: snx_ctr_fs_dispatch(&c); break;
        default: return SNX_CTR_SVC_UNSUPPORTED;
    }
    if (c.state == SNX_CTR_IPC_UNSUPPORTED) return SNX_CTR_SVC_UNSUPPORTED;
    if (c.state == SNX_CTR_IPC_BLOCKED) return SNX_CTR_SVC_BLOCKED;
    if (c.state == SNX_CTR_IPC_FAULT) return SNX_CTR_SVC_UNSUPPORTED;
    *result = c.transport_result;
    if (*result) return SNX_CTR_SVC_RETURN;
    for (unsigned i = 0; i < SNX_CTR_IPC_WORDS; ++i) store32(bytes + i * 4, c.out[i]);
    if (snx_ctr_write_ram(p->space, address, bytes, sizeof bytes) != SNX_CTR_MEM_OK) return SNX_CTR_SVC_UNSUPPORTED;
    return SNX_CTR_SVC_RETURN;
}
