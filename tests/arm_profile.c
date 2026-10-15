/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "cpu/arm/arm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint32_t forbidden[] = {0xe3013234, 0xe3453678, 0xe0634291, 0xe713f211, 0xe733f211,
                                         0xe6ff3f32, 0xe7e73051, 0xf57ff05f, 0xf57ff04f, 0xf57ff06f,
                                         0xe1910e9f, 0xe1810e92, 0xe1910c9f, 0xe181fc92, 0xe320f005,
                                         0xf4d1f000, 0xf591f000, 0xf2010802, 0xeef00a00, 0xeea00a00};
    for (size_t i = 0; i < sizeof forbidden / sizeof *forbidden; ++i) {
        assert(snx_a32_decode(forbidden[i]) != SNX_A32_UNDEFINED);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, forbidden[i]) == SNX_A32_UNDEFINED);
    }
    static const uint32_t valid[] = {0xe1a00000, 0xe6bf0f31, 0xe6af0071, 0xe16f0f11, 0xe1910f9f,
                                     0xe1810f92, 0xf57ff01f, 0xe320f004, 0xf5d0f000, 0xee1d0f70,
                                     0xee070fba, 0xee070f9a, 0xee070f95, 0xee300a01};
    for (size_t i = 0; i < sizeof valid / sizeof *valid; ++i)
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, valid[i]) != SNX_A32_UNDEFINED);
    assert(snx_a32_decode(0xee070fba) == SNX_A32_UNDEFINED);
    const uint32_t packed[] = {0xe6510f92, 0xe6810fb2};
    for (unsigned i = 0; i < 2; ++i) {
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i]) == (i ? SNX_A32_SEL : SNX_A32_UADD8));
        const unsigned shifts[] = {0, 12, 16};
        for (unsigned j = 0; j < 3; ++j)
            assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i] | (15u << shifts[j])) == SNX_A32_UNDEFINED);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i] | 0xf0000000u) == SNX_A32_UNDEFINED);
    }
    assert(snx_a32_decode_profile((SnxArmProfile)99, 0xe1a00000) == SNX_A32_UNDEFINED);
    uint32_t state = 1234;
    for (unsigned i = 0; i < 100000; ++i) {
        state = state * 1664525u + 1013904223u;
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A57, state) == snx_a32_decode(state));
    }
    for (unsigned i = 0; i <= 65535; ++i) {
        SnxThumb a = {0}, b = {0};
        unsigned x = snx_thumb_decode((uint16_t)i, 0xf800, &a);
        unsigned y = snx_thumb_decode_profile(SNX_ARM_PROFILE_A57, (uint16_t)i, 0xf800, &b);
        assert(x == y && !memcmp(&a, &b, sizeof a));
        y = snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, (uint16_t)i, 0xf800, &b);
        assert(y <= TH_F16); /* never accidentally decode Thumb-2 */
        if ((i & 0xff00) == 0xbf00 || ((i & 0xf800) == 0xe800 && (i & 1))) assert(!y);
    }
    SnxThumb t;
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xf400, 0xd000, &t) == 1 && t.shape == TH_LEGACY_BL);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xf800, 0, &t) == 1 && t.op == 1);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xe800, 0, &t) == 1 && t.op == 2);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xc903, 0, &t) == 1 && !(t.flags & TH_WB));
    puts("ARM11 profile and unchanged default decoder witnesses passed");
    return 0;
}
