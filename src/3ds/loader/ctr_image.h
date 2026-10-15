/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IMAGE_H
#define SNX_CTR_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Raw ExHeader values: .data bytes may include BSS in homebrew images.
   pages describe initialized page-packed .code; do not copy bytes blindly. */
typedef struct { uint32_t address, pages, bytes; } SnxCtrSegment;
typedef struct {
    SnxCtrSegment segment[3]; /* text, read-only, data; file offsets are page-packed */
    uint32_t bss_size, stack_size;
    uint64_t title_id;
    unsigned char identity[32]; /* SHA-256(domain || full ExHeader || expanded .code) */
    unsigned char* code;
    size_t code_size;
} SnxCtrImage;
/* Revalidate raw ExHeader and bounded, exact-sized .code, not an editable INI.
   Only use freshly extracted trees in trusted, non-concurrently-modified host
   directories. Identity is compatibility/integrity, NOT signature authenticity.
   On failure image is zeroed; free a successful image before reading another. */
bool snx_ctr_image_load(const char* directory, SnxCtrImage* image, char* error, size_t error_size);
void snx_ctr_image_free(SnxCtrImage* image);
#endif
