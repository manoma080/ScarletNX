/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "arm.h"
#include "simd.h"
#include <string.h>

/* Conservative VFPv2 recognition only. Emission deliberately stops with
   BACKEND_UNSUPPORTED: A57 FP helpers do not implement VFPv2 short vectors.
   Never silently execute NEON, D16..D31, VFPv3 immediates or fused operations. */
static int vfp2(uint32_t w) {
    SnxArmVector v;
    if ((w >> 28) == 15 || !snx_arm_vector_decode(w, &v) || (v.flags & AV_SIMD)) return 0;
    switch (v.op) {
        case AV_CORE: return (w & 0x0fe00f7f) == 0x0e000a10 && v.d < 128;
        case AV_CORE_PAIR: return v.d + 8 <= 128;
        case AV_STATUS: return 1;
        case AV_MEM: return v.d + v.bytes <= 128;
        case AV_FADD:
        case AV_FSUB:
        case AV_FMUL:
        case AV_FDIV:
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FNMUL: return v.d + v.bytes <= 128 && v.n + v.bytes <= 128 && v.m + v.bytes <= 128;
        case AV_MOV:
        case AV_FABS:
        case AV_FNEG:
        case AV_FSQRT: return v.d + v.bytes <= 128 && v.m + v.bytes <= 128;
        case AV_FCMP:
        case AV_FCMPE: return v.n + v.bytes <= 128 && (v.aux || v.m + v.bytes <= 128);
        case AV_FCVT:
            return (v.width == 32 || v.width == 64) && (v.aux == 32 || v.aux == 64) && v.d + v.bytes <= 128 &&
                   v.m + v.width / 8 <= 128;
        case AV_ITOF: return v.aux == 32 && !(v.imm >> 8) && v.d + v.bytes <= 128 && v.m + 4 <= 128;
        case AV_FTOI: return v.aux == 32 && !(v.imm >> 8) && v.d + v.bytes <= 128 && v.m + v.width / 8 <= 128;
        default: return 0;
    }
}
SnxA32Kind snx_a32_decode_profile(SnxArmProfile profile, uint32_t w) {
    if (profile == SNX_ARM_PROFILE_A57) return snx_a32_decode(w);
    if (profile != SNX_ARM_PROFILE_ARM11) return SNX_A32_UNDEFINED;
    /* ARM11 CP15 barrier operations, not ARMv7's unconditional encodings.
       Only these user-accessible maintenance operations are admitted. */
    uint32_t cp = w & 0x0fff0fff;
    if ((w >> 28) != 15 && ((w >> 12) & 15) != 15 && (cp == 0x0e070fba || cp == 0x0e070f9a || cp == 0x0e070f95))
        return SNX_A32_BARRIER;
    /* ARMv6 packed-byte operations used by libc. Keep the default A57
       decoder/emitter contract unchanged; admit only these exact ARM11 forms. */
    uint32_t packed = w & 0x0ff00ff0;
    if ((packed == 0x06500f90 || packed == 0x06800fb0) && (w >> 28) != 15) {
        if (((w >> 16) & 15) == 15 || ((w >> 12) & 15) == 15 || (w & 15) == 15) return SNX_A32_UNDEFINED;
        return packed == 0x06500f90 ? SNX_A32_UADD8 : SNX_A32_SEL;
    }
    SnxA32Kind kind = snx_a32_decode(w);
    switch (kind) {
        case SNX_A32_MOVW:
        case SNX_A32_MOVT:
        case SNX_A32_MLS:
        case SNX_A32_DIV:
        case SNX_A32_BITFIELD:
        case SNX_A32_ACQUIRE_RELEASE:
        case SNX_A32_BARRIER: return SNX_A32_UNDEFINED;
        case SNX_A32_REV: return (w & 0x0fff0ff0) == 0x06ff0f30 ? SNX_A32_UNDEFINED : kind; /* RBIT */
        case SNX_A32_EXCLUSIVE: return ((w >> 8) & 15) == 15 ? kind : SNX_A32_UNDEFINED; /* no ARMv8 acquire/release */
        case SNX_A32_HINT:
            if ((w >> 28) == 15) return (w & 0xfd70f000) == 0xf550f000 ? kind : SNX_A32_UNDEFINED; /* PLD */
            return (w & 15) <= 4 ? kind : SNX_A32_UNDEFINED;                                       /* no SEVL */
        case SNX_A32_VECTOR: return vfp2(w) ? kind : SNX_A32_UNDEFINED;
        default: return kind;
    }
}
unsigned snx_thumb_decode_profile(SnxArmProfile profile, uint16_t h, uint16_t h2, SnxThumb* o) {
    if (profile == SNX_ARM_PROFILE_A57) return snx_thumb_decode(h, h2, o);
    memset(o, 0, sizeof *o);
    if (profile != SNX_ARM_PROFILE_ARM11) return 0;
    /* ARMv6K Thumb BL is two separately executable 16-bit instructions. Its
       signed high offset is NOT Thumb-2's S/J1/J2 encoding. LR is observable
       after the prefix, and execution may enter directly at a suffix. */
    unsigned top = h & 0xf800;
    if (top == 0xf000 || top == 0xf800 || (top == 0xe800 && !(h & 1))) {
        o->shape = TH_LEGACY_BL;
        o->len = TH_F16;
        o->op = top == 0xf000 ? 0 : top == 0xf800 ? 1 : 2;
        o->imm = h & 0x7ff;
        return TH_F16;
    }
    if (top == 0x4800) {
        o->shape = TH_LOAD;
        o->len = TH_F16;
        o->rd = (h >> 8) & 7;
        o->rn = 15;
        o->imm = (h & 255) * 4;
        o->flags = TH_PCB | TH_PRE | TH_UP;
        return TH_F16;
    }
    if ((h & 0xff00) == 0xba00 && ((h >> 6) & 3) != 2) {
        o->shape = TH_REV;
        o->len = TH_F16;
        o->op = (h >> 6) & 3;
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        return TH_F16;
    }
    if ((h & 0xff00) == 0xbe00) {
        o->shape = TH_BRANCH;
        o->len = TH_F16;
        o->op = BR_BKPT;
        o->imm = h & 255;
        return TH_F16;
    }
    if (top >= 0xe800 || !snx_thumb_decode(h, h2, o)) goto invalid;
    if (o->shape == TH_IT || o->shape == TH_HINT ||
        (o->shape == TH_BRANCH && (o->op == BR_CBZ || o->op == BR_CBNZ || (o->op == BR_BLX_REG && o->rm == 15))))
        goto invalid;
    if (o->shape == TH_HI && o->op == 1 && (o->rd == 15 || o->rm == 15 || (o->rd < 8 && o->rm < 8))) goto invalid;
    if (o->shape == TH_PUSHPOP && !o->imm && !o->ra) goto invalid;
    if (o->shape == TH_LDMSTM) {
        if (!o->imm) goto invalid;
        if (o->imm & (1u << o->rn)) {
            if (o->op == 1)
                o->flags &= ~TH_WB; /* LDM base in list: load it, no writeback */
            else if (o->imm & ((1u << o->rn) - 1))
                goto invalid;
        }
    }
    return TH_F16;
invalid:
    memset(o, 0, sizeof *o);
    return 0;
}
