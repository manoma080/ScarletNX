/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PROCESS_INTERNAL_H
#define SNX_CTR_PROCESS_INTERNAL_H
#include "ctr_process.h"
#include "kernel/ctr_kernel.h"
#include "services/romfs.h"
#define CTR_HANDLES        256u
#define CTR_OBJECTS        256u
#define CTR_GSP_THREADS    4u
#define CTR_GSP_REGS       1024u
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
enum {
    CTR_OBJ_FREE,
    CTR_OBJ_PROCESS,
    CTR_OBJ_THREAD,
    CTR_OBJ_ARBITER,
    CTR_OBJ_SESSION,
    CTR_OBJ_MUTEX,
    CTR_OBJ_EVENT,
    CTR_OBJ_SHARED,
    CTR_OBJ_FILE
};
#define CTR_SELF_ARCHIVES 32u
/* FS-private read-only SelfNCCH bindings, never kernel handles. Tokens are
   monotonically issued and never reused; the process owns their RomFS backing. */
typedef struct {
    uint64_t token;
} CtrSelfArchive;
typedef struct {
    SnxArmCPU context; /* inactive architectural prefix only; never the checked adapter */
    uint32_t priority, dead, waiting_object, waiting_address;
    uint32_t wait_multiple, wait_all, wait_more_count, wait_more[63];
    char waiting_service[9];
} CtrThread;
typedef struct {
    uint32_t type, refs;
    union {
        struct {
            uint32_t service, client_pid, sdk_version, gsp_thread;
        } session;
        CtrThread thread;
        struct {
            uint32_t closed;
        } file;
        struct {
            uint32_t physical, size, permissions;
        } shared;
        struct {
            uint32_t owner, depth;
        } mutex;
        struct {
            uint32_t signaled, reset_type;
        } event; /* one-shot or sticky */
    } data;
} CtrObject;
typedef struct {
    uint32_t va, size, object;
} CtrSharedMap;
typedef struct {
    uint32_t generation, object;
} CtrHandle;
/* gsp::Gpu model: one shared GX command/interrupt block, a bounded GX/LCD
   register file, per-registration relay threads and latched buffer swaps.
   Display timing is host-paced 60 Hz, not cycle accurate. */
typedef struct {
    uint32_t shared_object;              /* CTR_OBJ_SHARED backing the 0x1000 block */
    uint32_t thread_mask, rights_thread; /* rights_thread 0xffffffff: no holder */
    uint32_t first_registration;         /* one genuine FirstInitialization reply */
    uint32_t lcd_black, frame_seq, vblank_count;
    uint32_t event_object[CTR_GSP_THREADS]; /* registered interrupt events (retained) */
    uint32_t relay_flags[CTR_GSP_THREADS];
    uint32_t fb_pending[2]; /* a SetBufferSwap latched since the last present */
    uint32_t fb_screen[2][7];
    uint64_t next_vblank_ns; /* host monotonic deadline, 60 Hz */
    uint32_t* regs;          /* bounded GX/LCD register file, CTR_GSP_REGS words */
} SnxCtrGsp;
struct SnxCtrProcess {
    SnxCtrRomfs* romfs; /* runtime-owned immutable host filesystem view */
    SnxCtrMemory* memory;
    SnxCtrAddressSpace* space;
    SnxCtrCpu cpu;
    uint32_t physical_next, physical_end, app_limit, physical_shared;
    CtrSharedMap shared_maps[8]; /* MapMemoryBlock registrations, kernel ordered */
    CtrObject objects[CTR_OBJECTS];
    CtrHandle handles[CTR_HANDLES];
    uint32_t last_svc, current_thread;
    uint32_t last_ipc_service, last_ipc_header;
    uint32_t apt_lock; /* manager-owned object index; independent of client sessions */
    uint32_t apt_phase, apt_notification, apt_parameter;
    uint32_t hid_objects[6];
    uint32_t shell_closed; /* virtual console starts with its shell open */
    double clock_origin;   /* host monotonic origin for the base ARM11 tick domain */
    CtrSelfArchive self_archives[CTR_SELF_ARCHIVES];
    uint64_t archive_sequence;
    SnxCtrGsp gsp;
    uint64_t svc_count;
    unsigned char physical_owner[];
};
CtrThread* snx_ctr_thread(SnxCtrProcess* p);
uint32_t snx_ctr_thread_create(SnxCtrProcess* p, const uint32_t* registers, uint32_t* out);
bool snx_ctr_schedule(SnxCtrProcess* p);
SnxCtrSvcResult snx_ctr_wait_many(SnxCtrProcess* p, uint32_t* registers);
void snx_ctr_wake_waiters(SnxCtrProcess* p, uint32_t object);
void snx_ctr_object_release(SnxCtrProcess* p, uint32_t object);
void snx_ctr_thread_block(SnxCtrProcess* p, uint32_t object, uint32_t address);
uint32_t snx_ctr_handle_object(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_handle_add(SnxCtrProcess* p, uint32_t object, uint32_t* out);
uint32_t snx_ctr_object_create(SnxCtrProcess* p, uint32_t type, uint32_t* out);
uint32_t snx_ctr_handle_close(SnxCtrProcess* p, uint32_t handle);
uint32_t snx_ctr_session_create(SnxCtrProcess* p, uint32_t service, uint32_t* out);
/* HLE shared pages live past the application pool but inside board FCRAM;
   each call reserves the next page-aligned block, or returns 0. */
uint32_t snx_ctr_shared_pages(SnxCtrProcess* p, uint32_t size);
uint32_t snx_ctr_named_connect(SnxCtrProcess* p, uint32_t name_address, uint32_t* out);
SnxCtrSvcResult snx_ctr_ipc_send(SnxCtrProcess* p, uint32_t handle, uint32_t* transport_result);
/* SVC helpers use only RAM transactions, never MMIO or a flat pointer. */
uint32_t snx_ctr_kernel_memory(SnxCtrProcess* p, uint32_t operation, uint32_t va, uint32_t source, uint32_t size,
                               uint32_t permissions, uint32_t* output);
#endif
