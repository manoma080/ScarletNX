/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_discover.h"
#include "ctr_module.h"
#include "cpu/arm/arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Resource limits apply to the offline graph, not to runtime compilation.
   Reject excessive graphs rather than silently publishing a truncated plan. */
#define INSTRUCTION_LIMIT 524288u
#define ENTRY_LIMIT       65536u
#define QUEUED            8u

typedef struct {
    const SnxCtrImage* image;
    SnxCtrPlan* plan;
    uint32_t *queue, count;
} Discovery;
static uint32_t little(const unsigned char* p, unsigned n) {
    uint32_t w = 0;
    for (unsigned i = 0; i < n; ++i) w |= (uint32_t)p[i] << (i * 8);
    return w;
}
static bool inside(const SnxCtrPlan* p, uint32_t pc, bool thumb) {
    unsigned width = thumb ? 2 : 4;
    return !(pc & (width - 1)) && pc >= p->address && (uint64_t)pc + width <= (uint64_t)p->address + p->bytes;
}
static unsigned char* flag(SnxCtrPlan* p, uint32_t pc, bool thumb) {
    return (thumb ? p->thumb : p->arm) + (pc - p->address) / (thumb ? 2 : 4);
}
static void enqueue(Discovery* d, uint32_t pc, bool thumb) {
    if (!inside(d->plan, pc, thumb)) return;
    unsigned char* f = flag(d->plan, pc, thumb);
    *f |= SNX_CTR_PLAN_ENTRY;
    if (!(*f & (QUEUED | SNX_CTR_PLAN_VISITED))) {
        *f |= QUEUED;
        d->queue[d->count++] = pc | (uint32_t)thumb;
    }
}
static void pointer(Discovery* d, uint32_t value) { enqueue(d, value & ~1u, !!(value & 1)); }
static bool read_word(const SnxCtrImage* image, uint32_t va, uint32_t* value) {
    size_t offset = 0;
    if (va & 3) return false;
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t n = (uint64_t)image->segment[i].pages * 4096;
        uint32_t base = image->segment[i].address;
        if (va >= base && (uint64_t)va + 4 <= (uint64_t)base + n && offset + n <= image->code_size) {
            *value = little(image->code + offset + (va - base), 4);
            return true;
        }
        offset += (size_t)n;
    }
    return false;
}
static uint32_t immediate(uint32_t w) {
    uint32_t v = w & 255;
    unsigned r = ((w >> 8) & 15) * 2;
    return r ? (v >> r) | (v << (32 - r)) : v;
}
/* Return whether this instruction terminates the emitted block. */
static bool arm(Discovery* d, uint32_t pc, uint32_t w) {
    SnxA32Kind k = snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, w);
    unsigned rd = (w >> 12) & 15, condition = w >> 28;
    bool conditional = condition < 14, stop = false, fall = conditional;
    if (k == SNX_A32_BRANCH || k == SNX_A32_BLX_IMM) {
        uint32_t off = (w & 0xffffff) << 2;
        if (off & 0x02000000) off |= 0xfc000000;
        if (k == SNX_A32_BLX_IMM) off |= (w >> 23) & 2;
        enqueue(d, pc + 8 + off, k == SNX_A32_BLX_IMM);
        stop = true;
        fall |= !!(w & (1u << 24)) || k == SNX_A32_BLX_IMM;
    } else if (k == SNX_A32_BX) {
        stop = true;
        fall |= !!(w & 0x20);
    } else if (k == SNX_A32_DATA) {
        unsigned op = (w >> 21) & 15, rn = (w >> 16) & 15;
        if ((w & (1u << 25)) && rn == 15 && (op == 2 || op == 4)) {
            uint32_t target = op == 4 ? pc + 8 + immediate(w) : pc + 8 - immediate(w);
            if (rd == 15)
                enqueue(d, target & ~3u, false); /* ARM11 ALU-to-PC does not interwork */
            else
                pointer(d, target);
        }
        stop = rd == 15;
        /* Bounded conservative coverage of PC-relative ADD jump tables, e.g.
           libgcc's integer division ladder. Out-of-window targets still stop. */
        if (stop && op == 4 && rn == 15 && !(w & (1u << 25)))
            for (unsigned i = 0; i < 128; ++i) enqueue(d, pc + 8 + i * 4, false);
    } else if (k == SNX_A32_SINGLE || k == SNX_A32_EXTRA) {
        stop = rd == 15 && !!(w & (1u << 20));
        /* CMP Rm,#N; LDRLS PC,[PC,Rm,LSL#2]. The adjacent unconditional
           unsigned bound proves a finite inline address table. No runtime
           target discovery, arbitrary text-pointer sweep or guessed bound. */
        uint32_t bound;
        if (k == SNX_A32_SINGLE && (w & 0xfffffff0u) == 0x979ff100u && pc >= d->plan->address + 4 &&
            read_word(d->image, pc - 4, &bound) && (bound & 0xfff0f000u) == 0xe3500000u &&
            ((bound >> 16) & 15) == (w & 15) && immediate(bound) < 256) {
            for (uint32_t i = 0; i <= immediate(bound); ++i) {
                uint32_t target;
                if (read_word(d->image, pc + 8 + i * 4, &target)) pointer(d, target);
            }
        }
        if (k == SNX_A32_SINGLE && ((w >> 16) & 15) == 15 && !(w & ((1u << 25) | (1u << 22))) && (w & (1u << 24)) &&
            (w & (1u << 20))) {
            uint32_t value, at = (w & (1u << 23)) ? pc + 8 + (w & 4095) : pc + 8 - (w & 4095);
            if (read_word(d->image, at, &value)) pointer(d, value);
        }
    } else if (k == SNX_A32_MULTIPLE)
        stop = (w & (1u << 15)) && (w & (1u << 20));
    else if (k == SNX_A32_SVC) {
        stop = true;
        fall |= (w & 0xffffff) != 3;
    } else if (k == SNX_A32_BKPT || k == SNX_A32_UNDEFINED || k == SNX_A32_VECTOR || k == SNX_A32_ACQUIRE_RELEASE)
        stop = true;
    if (stop && fall) enqueue(d, pc + 4, false);
    return stop;
}
static bool thumb(Discovery* d, uint32_t pc, uint16_t h) {
    SnxThumb t;
    if (!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, h, 0, &t)) return true;
    bool stop = false, fall = false;
    if (t.shape == TH_LEGACY_BL) {
        if (!t.op) {
            if (inside(d->plan, pc + 2, true)) {
                SnxThumb suffix;
                uint16_t next = (uint16_t)little(d->image->code + pc + 2 - d->plan->address, 2);
                if (snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, next, 0, &suffix) && suffix.shape == TH_LEGACY_BL &&
                    suffix.op) {
                    uint32_t off = t.imm << 12;
                    if (t.imm & 0x400) off |= 0xff800000;
                    uint32_t target = pc + 4 + off + (suffix.imm << 1);
                    enqueue(d, target & (suffix.op == 2 ? ~3u : ~1u), suffix.op != 2);
                }
            }
        } else {
            stop = true;
            fall = true;
        }
    } else if (t.shape == TH_BRANCH) {
        stop = true;
        if (t.op == BR_B || t.op == BR_BCOND) {
            uint32_t off = t.imm << 1;
            if (t.op == BR_B && (off & 0x800)) off |= 0xfffff000;
            if (t.op == BR_BCOND && (off & 0x100)) off |= 0xfffffe00;
            enqueue(d, pc + 4 + off, true);
            fall = t.op == BR_BCOND;
        } else if (t.op == BR_BLX_REG || (t.op == BR_SVC && t.imm != 3))
            fall = true;
    } else if (t.shape == TH_HI)
        stop = (t.op != 1 && t.rd == 15) || t.op == 3;
    else if (t.shape == TH_PUSHPOP)
        stop = t.op == 1 && t.ra;
    else if (t.shape == TH_LOAD && (t.flags & TH_PCB)) {
        uint32_t value;
        if (read_word(d->image, ((pc + 4) & ~3u) + t.imm, &value)) pointer(d, value);
    } else if (t.shape == TH_ADDR && t.rn == 15)
        pointer(d, ((pc + 4) & ~3u) + t.imm);
    if (stop && fall) enqueue(d, pc + 2, true);
    return stop;
}
void snx_ctr_plan_free(SnxCtrPlan* p) {
    if (p) {
        free(p->arm);
        free(p->thumb);
        memset(p, 0, sizeof *p);
    }
}
unsigned snx_ctr_plan_length(const SnxCtrPlan* p, uint32_t pc, bool thumb_mode) {
    if (!p || !p->arm || !p->thumb || !inside(p, pc, thumb_mode)) return 0;
    const unsigned char* flags = (thumb_mode ? p->thumb : p->arm);
    unsigned i = (pc - p->address) / (thumb_mode ? 2 : 4), limit = p->bytes / (thumb_mode ? 2 : 4), n = 0;
    if (!(flags[i] & SNX_CTR_PLAN_ENTRY)) return 0;
    while (i < limit && n < SNX_CTR_AOT_BLOCK_WORDS && (flags[i] & SNX_CTR_PLAN_VISITED)) {
        unsigned f = flags[i++];
        ++n;
        if ((f & SNX_CTR_PLAN_STOP) || (i < limit && (flags[i] & SNX_CTR_PLAN_ENTRY))) break;
    }
    return n;
}
bool snx_ctr_discover(const SnxCtrImage* image, SnxCtrPlan* p, char* error, size_t size) {
    if (!p) return false;
    memset(p, 0, sizeof *p);
    const char* reason = "invalid static discovery image";
    uint32_t* queue = NULL;
    if (!image || !image->code || !image->segment[0].bytes || (image->segment[0].bytes & 3) ||
        image->segment[0].bytes > SNX_CTR_AOT_TEXT_LIMIT || image->segment[0].bytes > image->code_size)
        goto fail;
    uint64_t packed = 0;
    for (unsigned i = 0; i < 3; ++i) packed += (uint64_t)image->segment[i].pages * 4096;
    if (packed != image->code_size || image->segment[0].bytes > (uint64_t)image->segment[0].pages * 4096 ||
        (image->segment[0].address & 4095) || image->segment[0].address < 0x100000 ||
        (uint64_t)image->segment[0].address + image->segment[0].bytes > 0x4000000)
        goto fail;
    p->address = image->segment[0].address;
    p->bytes = image->segment[0].bytes;
    uint32_t capacity = p->bytes / 4 + p->bytes / 2;
    p->arm = calloc(p->bytes / 4, 1);
    p->thumb = calloc(p->bytes / 2, 1);
    queue = malloc((size_t)capacity * sizeof *queue);
    reason = "out of memory for bounded static discovery";
    if (!p->arm || !p->thumb || !queue) goto fail;
    Discovery d = {image, p, queue, 0};
    enqueue(&d, p->address, false);
    /* Conservatively seed code pointers in initialized RO/data, e.g. vtables
       and constructors. This is static analysis, not relocation or runtime JIT. */
    size_t data = (size_t)image->segment[0].pages * 4096;
    for (size_t at = data; at + 4 <= image->code_size; at += 4) pointer(&d, little(image->code + at, 4));
    for (uint32_t head = 0; head < d.count; ++head) {
        bool t = !!(queue[head] & 1);
        uint32_t pc = queue[head] & ~1u;
        for (unsigned n = 0; n < SNX_CTR_AOT_BLOCK_WORDS && inside(p, pc, t); ++n) {
            unsigned char* f = flag(p, pc, t);
            if (*f & SNX_CTR_PLAN_VISITED) break;
            *f |= SNX_CTR_PLAN_VISITED;
            if (++p->instructions > INSTRUCTION_LIMIT) {
                reason = "static discovery instruction limit exceeded";
                goto fail;
            }
            uint32_t word = little(image->code + pc - p->address, t ? 2 : 4);
            bool stop = t ? thumb(&d, pc, (uint16_t)word) : arm(&d, pc, word);
            if (stop) {
                *f |= SNX_CTR_PLAN_STOP;
                break;
            }
            pc += t ? 2 : 4;
            if (n + 1 == SNX_CTR_AOT_BLOCK_WORDS) enqueue(&d, pc, t);
        }
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned char* f = mode ? p->thumb : p->arm;
        unsigned count = p->bytes / (mode ? 2 : 4);
        for (unsigned i = 0; i < count; ++i)
            if (f[i] & SNX_CTR_PLAN_ENTRY) ++p->entries;
    }
    if (p->entries > ENTRY_LIMIT) {
        reason = "static discovery entry limit exceeded";
        goto fail;
    }
    free(queue);
    return true;
fail:
    if (size) snprintf(error, size, "%s", reason);
    free(queue);
    snx_ctr_plan_free(p);
    return false;
}
