/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_image.h"
#include "common.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool read_exact(const char* root, const char* name, void* out, size_t size) {
    char* path = snx_path_join(root, name);
    FILE* f = !snx_is_link(path) && snx_is_file(path) ? snx_fopen(path, "rb") : NULL;
    free(path);
    if (!f) return false;
    bool ok = fread(out, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    if (fclose(f)) ok = false;
    return ok;
}
void snx_ctr_image_free(SnxCtrImage* image) {
    if (image) {
        free(image->code);
        memset(image, 0, sizeof *image);
    }
}
bool snx_ctr_image_load(const char* directory, SnxCtrImage* image, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (!image) return false;
    memset(image, 0, sizeof *image);
    unsigned char ex[0x800];
    const char* reason = "invalid extracted image directory or ExHeader";
    if (!directory || snx_is_link(directory) || !snx_is_dir(directory)) goto fail;
    char* exefs = snx_path_join(directory, "exefs");
    bool safe = !snx_is_link(exefs) && snx_is_dir(exefs);
    free(exefs);
    if (!safe || !read_exact(directory, "exheader.bin", ex, sizeof ex)) goto fail;
    image->stack_size = le32(ex + 0x1c);
    image->bss_size = le32(ex + 0x3c);
    image->title_id = (uint64_t)le32(ex + 0x200) | (uint64_t)le32(ex + 0x204) << 32;
    const unsigned offsets[] = {0x10, 0x20, 0x30};
    uint64_t sizes[3], total = 0;
    reason = "invalid extracted process segment/BSS/stack layout";
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned char* p = ex + offsets[i];
        SnxCtrSegment* s = &image->segment[i];
        *s = (SnxCtrSegment){le32(p), le32(p + 4), le32(p + 8)};
        uint64_t bytes = (uint64_t)s->pages * 4096;
        sizes[i] = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~UINT64_C(4095) : 0);
        /* Match extraction: some homebrew .data byte counts include BSS. */
        uint64_t initialized = s->bytes;
        if (i == 2 && initialized > bytes && initialized >= image->bss_size) initialized -= image->bss_size;
        if (initialized > bytes ||
            (sizes[i] && ((s->address & 4095) || s->address < 0x100000 || (uint64_t)s->address + sizes[i] > 0x4000000)))
            goto fail;
        for (unsigned j = 0; j < i; ++j)
            if (sizes[i] && sizes[j] && s->address < (uint64_t)image->segment[j].address + sizes[j] &&
                image->segment[j].address < (uint64_t)s->address + sizes[i])
                goto fail;
        total += bytes;
    }
    if (!image->segment[0].bytes || (image->segment[0].bytes & 3) || total > 0x4000000 || !image->stack_size ||
        image->stack_size > 0x800000)
        goto fail;
    image->code_size = (size_t)total;
    image->code = malloc(image->code_size);
    reason = "cannot read exact bounded expanded .code";
    if (!image->code || !read_exact(directory, "exefs/.code", image->code, image->code_size)) goto fail;
    static const char domain[] = "ScarletNX CTR image identity v1";
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    bool hashed = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
                  EVP_DigestUpdate(hash, domain, sizeof domain) == 1 && EVP_DigestUpdate(hash, ex, sizeof ex) == 1 &&
                  EVP_DigestUpdate(hash, image->code, image->code_size) == 1 &&
                  EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    reason = "cannot hash extracted image with OpenSSL";
    if (!hashed) goto fail;
    return true;
fail:
    if (error_size) snprintf(error, error_size, "%s", reason);
    snx_ctr_image_free(image);
    return false;
}
