/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   gsp::Gpu wire shapes and GX queue formats referenced against Azahar
   core/hle/service/gsp/* and libctru services/gspgpu.c. Bounded model: one
   shared GX/interrupt block per process, four relay threads, a checked GX/LCD
   register file and host-paced 60 Hz display timing. PICA command lists
   (SubmitCmdList), format-converting transfers and save-VRAM paths stop. */
#include "runtime/internal.h"
#include "services/gsp.h"
#include "gpu/present.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#define GSP_SHARED_SIZE        0x1000u
#define GSP_REG_BYTES          (CTR_GSP_REGS * 4u)
#define GSP_ERR_INVALID_HANDLE 0xd8e007f7u
#define GSP_ERR_OOM            0xd86007f3u
#define GSP_ERR_REGS_RANGE     0xe0e02a01u
#define GSP_ERR_REGS_SIZE      0xe0e02becu
#define GSP_ERR_REGS_ALIGN     0xe0e02bf2u
#define GSP_FIRST_INIT         0x2a07u
#define GSP_TIMED_OUT          0x09401bfeu
#define GSP_VBLANK_NS          UINT64_C(16666667)

static CtrSharedMap* shared_map(SnxCtrProcess* p, uint32_t object) {
    for (unsigned i = 0; i < 8; ++i)
        if (p->shared_maps[i].object == object) return &p->shared_maps[i];
    return NULL;
}
static void sleep_ns(uint64_t ns) {
#if defined(_WIN32)
    Sleep((DWORD)(ns / 1000000 ? ns / 1000000 : 1));
#else
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, NULL);
#endif
}
/* One interrupt relay slot per registered thread; the queue lives in the
   shared block the client maps. Full queues report the documented error byte
   and drop the interrupt instead of overwriting pending slots. */
static void queue_interrupt(SnxCtrProcess* p, uint32_t thread, uint32_t id) {
    CtrSharedMap* map = shared_map(p, p->gsp.shared_object);
    if (!map) return;
    uint32_t at = map->va + thread * 0x40;
    unsigned char header[4];
    if (snx_ctr_read_ram(p->space, at, header, sizeof header) != SNX_CTR_MEM_OK) return;
    unsigned next = (unsigned)(header[0] + header[1]) % 0x34;
    if (header[1] >= 0x34) {
        header[2] = 1;
        (void)snx_ctr_write_ram(p->space, at + 2, &header[2], 1);
        return;
    }
    unsigned char slot = (unsigned char)id;
    if (snx_ctr_write_ram(p->space, at + 8 + next, &slot, 1) != SNX_CTR_MEM_OK) return;
    ++header[1];
    (void)snx_ctr_write_ram(p->space, at + 1, &header[1], 1);
}
static void signal_registered(SnxCtrProcess* p, uint32_t thread) {
    uint32_t id = p->gsp.event_object[thread];
    if (!id) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
static uint32_t screen_height(uint32_t screen) { return screen ? 320u : 400u; }
static uint32_t bpp_of(uint32_t format) { return format == 0 ? 4u : format == 1 ? 3u : 2u; }
/* Present one latched buffer swap: the displayed half of the pair, read as
   guest RAM and shown through the Vulkan presenter. */
static void present_screen(SnxCtrProcess* p, uint32_t screen) {
    const uint32_t* fb = p->gsp.fb_screen[screen];
    uint32_t address = fb[5] ? fb[2] : fb[1], stride = fb[3], format = fb[4] & 7;
    if (!address || format > 4) return;
    uint32_t height = screen_height(screen), width = 240, bytes = bpp_of(format);
    if (stride < width * bytes || stride > 0x40000) return;
    uint64_t span = (uint64_t)stride * height;
    if (span > 8u * 1024 * 1024) return;
    unsigned char* pixels = malloc((size_t)span);
    if (!pixels) return;
    if (snx_ctr_check_ram(p->space, address, (size_t)span, SNX_CTR_READ) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, address, pixels, (size_t)span) != SNX_CTR_MEM_OK) {
        /* The configured framebuffer cannot be read: stop explicitly rather
           than present stale or fabricated pixels. */
        free(pixels);
        p->gsp.fb_pending[screen] = 0;
        return;
    }
    uint64_t hash = 0;
    if (!snx_ctr_present_frame(screen, width, height, format, stride, pixels, &hash)) {
        free(pixels);
        return;
    }
    free(pixels);
    p->gsp.fb_pending[screen] = 0;
    ++p->gsp.frame_seq;
}
/* Host-paced display timing: one vblank delivers both PDC interrupts to every
   registered relay thread and shows latched frames. Not cycle accurate. */
void snx_ctr_gsp_vblank(SnxCtrProcess* p) {
    if (!p->gsp.shared_object) return;
    ++p->gsp.vblank_count;
    p->gsp.next_vblank_ns += GSP_VBLANK_NS;
    for (uint32_t screen = 0; screen < 2; ++screen)
        if (p->gsp.fb_pending[screen]) present_screen(p, screen);
    for (uint32_t thread = 0; thread < CTR_GSP_THREADS; ++thread) {
        if (!p->gsp.event_object[thread]) continue;
        queue_interrupt(p, thread, 2); /* PDC0 */
        queue_interrupt(p, thread, 3); /* PDC1 */
        signal_registered(p, thread);
    }
}
bool snx_ctr_gsp_event(const SnxCtrProcess* p, uint32_t object) {
    if (!object) return false;
    for (uint32_t thread = 0; thread < CTR_GSP_THREADS; ++thread)
        if (p->gsp.event_object[thread] == object) return true;
    return false;
}
/* Waits on the registered interrupt event complete at the next real vblank
   delivery. Immediate and infinite forms only; finite deadlines stop. */
SnxCtrSvcResult snx_ctr_gsp_wait(SnxCtrProcess* p, uint32_t object, uint32_t* r) {
    if (!r[2] && !r[3]) {
        r[0] = GSP_TIMED_OUT;
        return SNX_CTR_SVC_RETURN;
    }
    if (r[2] != UINT32_MAX || r[3] != UINT32_MAX) return SNX_CTR_SVC_UNSUPPORTED;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        double now = snx_seconds() * 1e9;
        uint64_t deadline = p->gsp.next_vblank_ns;
        if (deadline > (uint64_t)now) sleep_ns(deadline - (uint64_t)now);
        snx_ctr_gsp_vblank(p);
        CtrObject* o = &p->objects[object];
        if (o->data.event.signaled) {
            if (!o->data.event.reset_type) o->data.event.signaled = 0;
            r[0] = 0;
            return SNX_CTR_SVC_RETURN;
        }
    }
    return SNX_CTR_SVC_UNSUPPORTED;
}
static uint32_t* regs(SnxCtrProcess* p) {
    if (!p->gsp.regs) p->gsp.regs = calloc(CTR_GSP_REGS, 4);
    return p->gsp.regs;
}
static void write_regs(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t at = c->in[1], size = c->in[2];
    if ((at & 3) || (size & 3) || size > 512 || (uint64_t)at + size > GSP_REG_BYTES) {
        snx_ctr_ipc_error_reply(c, (at & 3)     ? GSP_ERR_REGS_ALIGN
                                   : (size & 3) ? GSP_ERR_REGS_ALIGN
                                   : size > 512 ? GSP_ERR_REGS_SIZE
                                                : GSP_ERR_REGS_RANGE);
        return;
    }
    if (c->in[3] != ((size << 14) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char data[512];
    if (snx_ctr_read_ram(p->space, c->in[4], data, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t* file = regs(p);
    if (!file) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
        return;
    }
    memcpy((unsigned char*)file + at, data, size);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void write_regs_mask(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 4)) return;
    SnxCtrProcess* p = c->process;
    uint32_t at = c->in[1], size = c->in[2];
    if ((at & 3) || (size & 3) || size > 512 || (uint64_t)at + size > GSP_REG_BYTES) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
        return;
    }
    if (c->in[3] != ((size << 14) | 2) || c->in[5] != ((size << 14) | (1u << 11) | 2)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char data[512], mask[512];
    if (snx_ctr_read_ram(p->space, c->in[4], data, size) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, c->in[6], mask, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t* file = regs(p);
    if (!file) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
        return;
    }
    uint32_t words = size / 4;
    uint32_t* target = (uint32_t*)((unsigned char*)file + at);
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t v = 0, m = 0;
        memcpy(&v, data + i * 4, 4);
        memcpy(&m, mask + i * 4, 4);
        target[i] = (target[i] & ~m) | (v & m);
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void set_buffer_swap(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 0)) return;
    SnxCtrProcess* p = c->process;
    /* words: screen, then FrameBufferInfo {active_fb, address_left,
       address_right, stride, format, shown_fb, unknown} */
    uint32_t screen = c->in[1], stride = c->in[5], format = c->in[6] & 7, shown = c->in[7];
    if (screen > 1 || format > 4) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* Only the bit layouts the presenter converts are accepted; the rest stop
       instead of being displayed with invented colors. */
    if (format != 0 && format != 2) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (shown > 1 || (uint64_t)stride * screen_height(screen) > 8u * 1024 * 1024) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
        return;
    }
    for (unsigned i = 0; i < 7; ++i) p->gsp.fb_screen[screen][i] = c->in[2 + i];
    p->gsp.fb_pending[screen] = 1;
    /* A swap schedules the frame for the next real vblank; if display time has
       already passed while the guest ran, service it now. */
    if (p->gsp.shared_object && (uint64_t)(snx_seconds() * 1e9) >= p->gsp.next_vblank_ns) snx_ctr_gsp_vblank(p);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void cache_op(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    /* Host memory is coherent with guest reads; a data cache flush/invalidate
       is a genuine no-op in this model. The process handle must be real. */
    if (c->handle_in[0] != CTR_PROCESS_HANDLE) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_INVALID_HANDLE);
        return;
    }
    if ((uint64_t)c->in[1] + c->in[2] > UINT64_C(0x100000000)) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
        return;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void register_relay(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    CtrObject* session = &p->objects[c->object];
    uint32_t event = snx_ctr_handle_object(p, c->handle_in[0]);
    if (!event || p->objects[event].type != CTR_OBJ_EVENT) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_INVALID_HANDLE);
        return;
    }
    if (!p->gsp.regs && !regs(p)) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
        return;
    }
    if (!p->gsp.shared_object) {
        uint32_t out = 0;
        uint32_t physical = snx_ctr_shared_pages(p, GSP_SHARED_SIZE);
        uint32_t result = physical ? snx_ctr_object_create(p, CTR_OBJ_SHARED, &out) : GSP_ERR_OOM;
        if (result) {
            snx_ctr_ipc_error_reply(c, result);
            return;
        }
        uint32_t id = snx_ctr_handle_object(p, out);
        p->objects[id].data.shared.physical = physical;
        p->objects[id].data.shared.size = GSP_SHARED_SIZE;
        p->objects[id].data.shared.permissions = 3;
        p->gsp.shared_object = id;
        p->gsp.rights_thread = UINT32_MAX;
        p->gsp.next_vblank_ns = (uint64_t)(snx_seconds() * 1e9) + GSP_VBLANK_NS;
        /* The block keeps its own reference while the process holds it. */
        ++p->objects[id].refs;
    }
    uint32_t thread = session->data.session.gsp_thread;
    if (thread == UINT32_MAX) {
        for (uint32_t i = 0; i < CTR_GSP_THREADS; ++i) {
            if (p->gsp.thread_mask & (1u << i)) continue;
            thread = i;
            p->gsp.thread_mask |= 1u << i;
            break;
        }
        if (thread == UINT32_MAX) {
            snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
            return;
        }
        session->data.session.gsp_thread = thread;
    }
    if (!p->gsp.event_object[thread]) {
        ++p->objects[event].refs;
        p->gsp.event_object[thread] = event;
        p->gsp.relay_flags[thread] = c->in[1];
    }
    uint32_t shared_handle = 0;
    uint32_t result = snx_ctr_handle_add(p, p->gsp.shared_object, &shared_handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    c->out[0] = 0x00130082;
    c->out[1] = p->gsp.first_registration ? 0 : GSP_FIRST_INIT;
    p->gsp.first_registration = 1;
    c->out[2] = thread;
    c->out[3] = 0x10;
    snx_ctr_ipc_output_handle(c, 4, shared_handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, shared_handle);
}
static void unregister_relay(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t thread = p->objects[c->object].data.session.gsp_thread;
    if (thread != UINT32_MAX && p->gsp.event_object[thread]) {
        snx_ctr_object_release(p, p->gsp.event_object[thread]);
        p->gsp.event_object[thread] = 0;
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void acquire(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    if (c->handle_in[0] != CTR_PROCESS_HANDLE || c->in[1] > 7) {
        snx_ctr_ipc_error_reply(c, c->handle_in[0] != CTR_PROCESS_HANDLE ? GSP_ERR_INVALID_HANDLE : GSP_ERR_REGS_RANGE);
        return;
    }
    /* One bounded right owner: the acquiring session, as on hardware. */
    p->gsp.rights_thread = c->object;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
static void release_right(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    if (p->gsp.rights_thread != UINT32_MAX && p->gsp.rights_thread != c->object) {
        snx_ctr_ipc_error_reply(c, SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    p->gsp.rights_thread = UINT32_MAX;
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
/* GX command queue in the shared block: DMA copies and memory fills run as
   checked RAM transactions; cache flushes are coherent no-ops. PICA command
   lists and format-converting transfers stop explicitly. */
static void trigger_queue(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    CtrSharedMap* map = p->gsp.shared_object ? shared_map(p, p->gsp.shared_object) : NULL;
    if (p->gsp.rights_thread == UINT32_MAX) {
        c->out[0] = (c->in[0] & 0xffff0000u) | 0x40; /* succeeds with no active queue */
        return;
    }
    uint32_t thread = p->objects[p->gsp.rights_thread].data.session.gsp_thread;
    if (!map || thread == UINT32_MAX || thread >= CTR_GSP_THREADS) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char queue[0x200];
    uint32_t at = map->va + 0x800 + thread * 0x200;
    if (snx_ctr_read_ram(p->space, at, queue, sizeof queue) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    unsigned index = queue[0], pending = queue[1];
    if (pending > 15) {
        snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
        return;
    }
    while (pending && !queue[3]) {
        if (queue[2] == 1) break;
        const unsigned char* cmd = queue + 0x20 + (index % 15) * 0x20;
        uint32_t id = cmd[0];
        index = (index + 1) % 15;
        --pending;
        if (id == 0) { /* RequestDma: one checked RAM copy */
            uint32_t args[3];
            memcpy(args, cmd + 4, sizeof args);
            uint64_t span = args[2];
            if (span > 16u * 1024 * 1024 || !span) {
                c->state = SNX_CTR_IPC_UNSUPPORTED;
                return;
            }
            unsigned char* staged = malloc((size_t)span);
            if (!staged) {
                snx_ctr_ipc_error_reply(c, GSP_ERR_OOM);
                return;
            }
            if (snx_ctr_check_ram(p->space, args[0], (size_t)span, SNX_CTR_READ) != SNX_CTR_MEM_OK ||
                snx_ctr_check_ram(p->space, args[1], (size_t)span, SNX_CTR_WRITE) != SNX_CTR_MEM_OK ||
                snx_ctr_read_ram(p->space, args[0], staged, (size_t)span) != SNX_CTR_MEM_OK ||
                snx_ctr_write_ram(p->space, args[1], staged, (size_t)span) != SNX_CTR_MEM_OK) {
                free(staged);
                c->state = SNX_CTR_IPC_FAULT;
                return;
            }
            free(staged);
        } else if (id == 2) { /* MemoryFill: two checked fill regions */
            uint32_t args[6];
            memcpy(args, cmd + 4, sizeof args);
            for (unsigned region = 0; region < 2; ++region) {
                uint32_t start = args[region * 3], value = args[region * 3 + 1], end = args[region * 3 + 2];
                if (!start && !end) continue;
                end &= 0x7fffffffu; /* the width flag lives in the high bit */
                uint64_t span = (uint64_t)end - start;
                if (end <= start || (start & 3) || (end & 3) || span > 32u * 1024 * 1024 ||
                    snx_ctr_check_ram(p->space, start, (size_t)span, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
                    c->state = SNX_CTR_IPC_FAULT;
                    return;
                }
                for (uint32_t at4 = start; at4 < end; at4 += 4)
                    if (snx_ctr_write_ram(p->space, at4, &value, 4) != SNX_CTR_MEM_OK) {
                        c->state = SNX_CTR_IPC_FAULT;
                        return;
                    }
            }
        } else if (id == 5) {
            /* CacheFlush: genuine no-op on coherent host memory. */
        } else if (id == 1 || id == 3 || id == 4) {
            /* SubmitCmdList (PICA), DisplayTransfer, TextureCopy: stop. */
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        } else {
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
        }
        if (cmd[2]) queue[2] = 1; /* stop flag on the processed command */
    }
    queue[0] = (unsigned char)index;
    queue[1] = (unsigned char)pending;
    if (snx_ctr_write_ram(p->space, at, queue, 4) != SNX_CTR_MEM_OK) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    if (p->gsp.shared_object) {
        if (pending == 0) {
            queue_interrupt(p, thread, 0); /* PSC0/PSC1 and PPF completed */
            queue_interrupt(p, thread, 1);
            queue_interrupt(p, thread, 4);
            signal_registered(p, thread);
        }
    }
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
}
void snx_ctr_gsp_dispatch(SnxCtrIpc* c) {
    switch (c->in[0] >> 16) {
        case 0x001: write_regs(c); break;
        case 0x002: write_regs_mask(c); break;
        case 0x005: set_buffer_swap(c); break;
        case 0x008:
        case 0x009: cache_op(c); break;
        case 0x00b:
            if (!snx_ctr_ipc_expect(c, 1, 0) || c->in[1] > 1) {
                if (c->state != SNX_CTR_IPC_RETURN) return;
                snx_ctr_ipc_error_reply(c, GSP_ERR_REGS_RANGE);
                return;
            }
            c->process->gsp.lcd_black = c->in[1];
            c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
            break;
        case 0x00c: trigger_queue(c); break;
        case 0x013: register_relay(c); break;
        case 0x014: unregister_relay(c); break;
        case 0x015:
        case 0x016: acquire(c); break;
        case 0x017: release_right(c); break;
        default: c->state = SNX_CTR_IPC_UNSUPPORTED; break;
    }
}
