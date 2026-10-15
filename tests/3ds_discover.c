/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "aot/ctr_discover.h"
#include "aot/ctr_module.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(unsigned char* p, uint32_t w) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(w >> (i * 8)); }
static void half(unsigned char* p, uint16_t h) { p[0] = (unsigned char)h; p[1] = (unsigned char)(h >> 8); }
int main(void) {
    char error[512]; SnxCtrPlan p;
    SnxCtrImage image = {.code_size = 0x7000,
        .segment = {{0x100000, 5, 0x5000}, {0x200000, 1, 64}, {0x300000, 1, 4}}};
    image.code = calloc(1, image.code_size); assert(image.code);
    put(image.code, 0xeb000006); put(image.code + 4, 0xef000003);
    put(image.code + 0x20, 0xe59f3008); put(image.code + 0x24, 0xe12fff13);
    put(image.code + 0x30, 0x101001); /* ARM literal -> Thumb indirect target */
    half(image.code + 0x1000, 0x2007); half(image.code + 0x1002, 0xf000);
    half(image.code + 0x1004, 0xf80d); half(image.code + 0x1006, 0xdf03);
    half(image.code + 0x1020, 0x4770);
    put(image.code + 0x5000, 0x100100); put(image.code + 0x5004, 0x102000);
    put(image.code + 0x100, 0x13010234); put(image.code + 0x104, 0xef000003); /* conditional unsupported MOVW */
    put(image.code + 0x6000, 0x101800); put(image.code + 0x1800, 0xe08ff103);
    for (unsigned i = 0; i < 128; ++i) put(image.code + 0x1808 + i * 4, 0xef000003);
    for (unsigned i = 0; i < 64; ++i) put(image.code + 0x2000 + i * 4, 0xe1a00000);
    put(image.code + 0x2100, 0xef000003);
    const uint32_t roots[] = {0x100200, 0x100240, 0x100300, 0x100500, 0x100600, 0x100a01};
    for (unsigned i = 0; i < sizeof roots / sizeof *roots; ++i) put(image.code + 0x5010 + i * 4, roots[i]);
    put(image.code + 0x200, 0xfa00007e); put(image.code + 0x204, 0xef000003); /* immediate BLX -> Thumb */
    half(image.code + 0x400, 0xdf03);
    put(image.code + 0x240, 0xe28f0001); put(image.code + 0x244, 0xef000003); half(image.code + 0x248, 0xdf03);
    put(image.code + 0x300, 0xeaffffbe); /* negative ARM B -> 0x100200 */
    put(image.code + 0x500, 0xe28ff001); put(image.code + 0x508, 0xef000003); /* ALU-to-PC stays ARM */
    put(image.code + 0x5fc, 0x100701); put(image.code + 0x600, 0xe51f300c); put(image.code + 0x604, 0xe12fff13);
    half(image.code + 0x700, 0xdf03);
    half(image.code + 0xa00, 0xf7ff); half(image.code + 0xa02, 0xfefe); /* negative legacy BL */
    half(image.code + 0xa04, 0xdf03); half(image.code + 0x800, 0x4770);
    assert(snx_ctr_discover(&image, &p, error, sizeof error));
    assert(p.entries > 128 && p.instructions > p.entries);
    assert(snx_ctr_plan_length(&p, 0x100000, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x100004, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x100020, false) == 2);
    assert(snx_ctr_plan_length(&p, 0x101000, true) == 3);
    assert(snx_ctr_plan_length(&p, 0x101006, true) == 1);
    assert(snx_ctr_plan_length(&p, 0x101020, true) == 1);
    assert(snx_ctr_plan_length(&p, 0x100100, false) == 1 && snx_ctr_plan_length(&p, 0x100104, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x102000, false) == 32 && snx_ctr_plan_length(&p, 0x102080, false) == 32);
    assert(snx_ctr_plan_length(&p, 0x101808, false) == 1 && snx_ctr_plan_length(&p, 0x101a04, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x100400, true) == 1 && snx_ctr_plan_length(&p, 0x100204, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x100248, true) == 1 && snx_ctr_plan_length(&p, 0x100300, false) == 1);
    assert(snx_ctr_plan_length(&p, 0x100508, false) == 1 && !snx_ctr_plan_length(&p, 0x100508, true));
    assert(snx_ctr_plan_length(&p, 0x100600, false) == 2 && snx_ctr_plan_length(&p, 0x100700, true) == 1);
    assert(snx_ctr_plan_length(&p, 0x100800, true) == 1 && snx_ctr_plan_length(&p, 0x100a04, true) == 1);
    assert(!snx_ctr_plan_length(&p, 0x100008, false)); /* no indiscriminate linear data sweep */
    assert(!snx_ctr_plan_length(&p, 0x100001, false) && !snx_ctr_plan_length(&p, 0xffffffff, true));
    snx_ctr_plan_free(&p); assert(!p.arm && !p.thumb && !snx_ctr_plan_length(&p, 0x100000, false));
    image.segment[0].bytes = SNX_CTR_AOT_TEXT_LIMIT + 4;
    assert(!snx_ctr_discover(&image, &p, error, sizeof error) && !p.arm);
    image.segment[0].bytes = 0x5000; image.segment[0].address += 2;
    assert(!snx_ctr_discover(&image, &p, error, sizeof error)); image.segment[0].address -= 2;
    --image.code_size; assert(!snx_ctr_discover(&image, &p, error, sizeof error)); free(image.code);
    /* Exhausting the analysis budget is an error, not silent partial coverage. */
    image = (SnxCtrImage){.code_size = 0x201000, .segment = {{0x100000, 513, 0x200004}}};
    image.code = calloc(1, image.code_size); assert(image.code);
    assert(!snx_ctr_discover(&image, &p, error, sizeof error) && strstr(error, "instruction limit") && !p.arm);
    free(image.code);
    puts("CTR bounded static CFG discovery / literal-pointer / jump-window / resource tests passed");
    return 0;
}
