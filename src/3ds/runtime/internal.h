/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PROCESS_INTERNAL_H
#define SNX_CTR_PROCESS_INTERNAL_H
#include "ctr_process.h"
#include "kernel/ctr_kernel.h"
#define CTR_HANDLES        256u
#define CTR_OBJECTS        256u
#define CTR_PHYS_BASE      0x20000000u
#define CTR_CONFIG         0x1ff80000u
#define CTR_HEAP           0x08000000u
#define CTR_HEAP_END       0x10000000u
#define CTR_LINEAR         0x14000000u
#define CTR_PROCESS_HANDLE 0xffff8001u
#define CTR_THREAD_HANDLE  0xffff8000u
/* Physical page ownership, not a replacement for the checked page controller. */
enum { CTR_UNUSED, CTR_STATIC, CTR_HEAP_PAGE, CTR_LINEAR_PAGE };
#define CTR_PROCESS_ID 0x10u
enum { CTR_OBJ_FREE, CTR_OBJ_PROCESS, CTR_OBJ_THREAD, CTR_OBJ_ARBITER, CTR_OBJ_SESSION };
typedef struct {
    uint32_t type, refs;
    union {
        struct {
            uint32_t service, client_pid, sdk_version;
        } session;
    } data;
} CtrObject;
typedef struct {
    uint32_t generation, object;
} CtrHandle;
struct SnxCtrProcess {
    SnxCtrMemory* memory;
    SnxCtrAddressSpace* space;
    SnxCtrCpu cpu;
    uint32_t physical_next, physical_end, app_limit;
    CtrObject objects[CTR_OBJECTS];
    CtrHandle handles[CTR_HANDLES];
    uint32_t last_svc, waiting_object, waiting_address;
    uint32_t last_ipc_service, last_ipc_header;
    char waiting_service[9];
    uint64_t svc_count;
    unsigned char physical_owner[];
};
uint32_t snx_ctr_handle_object(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_object_create(SnxCtrProcess* p, uint32_t type, uint32_t* out);
uint32_t snx_ctr_handle_close(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_session_create(SnxCtrProcess* p, uint32_t service, uint32_t* out);
uint32_t snx_ctr_named_connect(SnxCtrProcess* p, uint32_t name_address, uint32_t* out);
SnxCtrSvcResult snx_ctr_ipc_send(SnxCtrProcess* p, uint32_t handle, uint32_t* transport_result);
/* SVC helpers use only RAM transactions, never MMIO or a flat pointer. */
uint32_t snx_ctr_kernel_memory(SnxCtrProcess* p, uint32_t operation, uint32_t va, uint32_t source, uint32_t size,
                               uint32_t permissions, uint32_t* output);
#endif
