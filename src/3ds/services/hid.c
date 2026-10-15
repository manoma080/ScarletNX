/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Initial HID shared memory/events only. No fabricated samples or completed
   input waits; periodic sampling/host input are not implemented here. */
#include "runtime/internal.h"
#include "services/hid.swipc.inc"
static uint32_t snx_ctr_hid_GetIPCHandles(SnxCtrIpc* c, uint32_t* memory, uint32_t* pad1, uint32_t* pad2,
                                          uint32_t* accelerometer, uint32_t* gyroscope, uint32_t* debug_pad) {
    SnxCtrProcess* p = c->process;
    uint32_t* out[] = {memory, pad1, pad2, accelerometer, gyroscope, debug_pad};
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t result = p->hid_objects[0] ? snx_ctr_handle_add(p, p->hid_objects[i], out[i])
                                            : snx_ctr_object_create(p, i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED, out[i]);
        if (result) return result; /* generated output cleanup is transactional */
    }
    if (!p->hid_objects[0]) {
        for (unsigned i = 0; i < 6; ++i) {
            p->hid_objects[i] = snx_ctr_handle_object(p, *out[i]);
            ++p->objects[p->hid_objects[i]].refs;
        }
        /* One HLE system-pool page, outside the application pool/accounting.
           Board RAM is zero-initialized: no samples/events have been produced. */
        CtrObject* shared = &p->objects[p->hid_objects[0]];
        shared->data.shared.physical = p->physical_end;
        shared->data.shared.size = 4096;
        shared->data.shared.permissions = SNX_CTR_READ;
    }
    return 0;
}
