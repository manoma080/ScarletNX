/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format references: Azahar ncch_container.h/.cpp and loader/ncch.cpp,
   68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e, GPL-3.0-or-later. */
#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t address, pages, bytes;
} Segment;
static bool extract_ncch(CtrReader* r, const CtrCia* cia, const char* stage, unsigned char** stored,
                         unsigned char** code) {
    unsigned char h[0x200], exheader[0x800], exefs[0x200];
    if (!ctr_read(r, cia->boot, 0, h, sizeof h)) return false;
    if (memcmp(h + 0x100, "NCCH", 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "boot content is not NCCH");
    if (h[0x18e] || h[0x18c] != 1)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "unsupported NCCH media unit/platform");
    if (!(h[0x18f] & 4)) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "encrypted NCCH is not supported");
    if (!(h[0x18d] & 2) || ((h[0x18d] >> 2) & 7))
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "boot NCCH is not an executable application");
    if ((uint64_t)ctr_le32(h + 0x104) * 512 != cia->boot.size || ctr_le64(h + 0x118) != cia->title_id ||
        ctr_le32(h + 0x180) != 0x400)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "NCCH size/title/ExHeader mismatch");
    CtrSpan regions[4];
    const unsigned offsets[] = {0x190, 0x198, 0x1a0, 0x1b0};
    for (size_t i = 0; i < 4; ++i) {
        uint64_t at = (uint64_t)ctr_le32(h + offsets[i]) * 512;
        uint64_t size = (uint64_t)ctr_le32(h + offsets[i] + 4) * 512;
        if ((size && at < 0xa00) || !ctr_fits(cia->boot.size, at, size))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid NCCH section range");
        regions[i] = (CtrSpan){cia->boot.offset + at, size};
        for (size_t j = 0; size && j < i; ++j)
            if (regions[j].size && regions[i].offset < regions[j].offset + regions[j].size &&
                regions[j].offset < regions[i].offset + size)
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping NCCH sections");
    }
    CtrSpan exe = regions[2], rom = regions[3];
    if (!exe.size || (!!rom.size == !!(h[0x18f] & 2)))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "missing ExeFS or inconsistent RomFS flag");
    uint64_t hash_size = (uint64_t)ctr_le32(h + 0x1a8) * 512;
    if (hash_size < sizeof exefs || hash_size > exe.size ||
        !ctr_hash_matches(r, (CtrSpan){exe.offset, hash_size}, h + 0x1c0))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS superblock hash/range");
    if (!ctr_read(r, cia->boot, 0x200, exheader, sizeof exheader)) return false;
    unsigned char hash[32];
    if (!ctr_sha256(r, exheader, 0x400, hash)) return false;
    if (memcmp(hash, h + 0x160, 32) || ctr_le64(exheader + 0x200) != cia->title_id)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ExHeader hash/program ID mismatch");
    Segment segments[3];
    const unsigned segment_offsets[] = {0x10, 0x20, 0x30};
    uint64_t expected = 0, minimum = 0;
    uint32_t bss = ctr_le32(exheader + 0x3c), stack = ctr_le32(exheader + 0x1c);
    for (size_t i = 0; i < 3; ++i) {
        const unsigned char* p = exheader + segment_offsets[i];
        Segment* s = &segments[i];
        *s = (Segment){ctr_le32(p), ctr_le32(p + 4), ctr_le32(p + 8)};
        uint64_t length = (uint64_t)s->pages * 4096;
        uint64_t virtual_length = length + (i == 2 ? ((uint64_t)bss + 4095) / 4096 * 4096 : 0);
        /* Some homebrew makers report .data size INCLUDING BSS, while pages
           describe only initialized data. Never materialize BSS from .code. */
        uint64_t initialized = s->bytes;
        if (i == 2 && initialized > length && initialized >= bss) initialized -= bss;
        if (initialized > length || (virtual_length && ((s->address & 4095) || s->address < 0x100000 ||
            !ctr_fits(0x4000000, s->address, virtual_length))))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExHeader segment/BSS range");
        for (size_t j = 0; virtual_length && j < i; ++j) {
            uint64_t end = (uint64_t)segments[j].address + (uint64_t)segments[j].pages * 4096;
            uint64_t this_end = (uint64_t)s->address + virtual_length;
            if (segments[j].pages && s->address < end && segments[j].address < this_end)
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping process segments");
        }
        minimum = expected + initialized;
        expected += length;
    }
    if (!segments[0].bytes || !segments[0].pages || expected > CTR_CODE_LIMIT || !stack || stack > 0x800000)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid code/stack/BSS sizes");
    if (!ctr_read(r, exe, 0, exefs, sizeof exefs)) return false;
    char names[10][9] = {{0}};
    CtrSpan files[10] = {{0}};
    int code_index = -1;
    for (size_t i = 0; i < 10; ++i) {
        const unsigned char* p = exefs + i * 16;
        if (!*p) {
            for (size_t k = 0; k < 16; ++k)
                if (p[k]) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid empty ExeFS entry");
            continue;
        }
        bool terminated = false;
        for (size_t k = 0; k < 8; ++k) {
            if (p[k] >= 128 || (terminated && p[k]))
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS name encoding/padding");
            if (!p[k]) terminated = true;
        }
        memcpy(names[i], p, 8);
        if (!ctr_name(names[i])) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "unsafe ExeFS filename");
        uint64_t at = (uint64_t)ctr_le32(p + 8) + 0x200, size = ctr_le32(p + 12);
        if (!ctr_fits(exe.size, at, size)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ExeFS file exceeds section");
        files[i] = (CtrSpan){exe.offset + at, size};
        for (size_t j = 0; j < i; ++j)
            if (*names[j] && (!strcmp(names[i], names[j]) ||
                              (files[i].size && files[j].size && files[i].offset < files[j].offset + files[j].size &&
                               files[j].offset < files[i].offset + size)))
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate or overlapping ExeFS files");
        if (!strcmp(names[i], ".code")) code_index = (int)i;
    }
    if (code_index < 0 || files[code_index].size > CTR_CODE_LIMIT || !files[code_index].size)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "missing or oversized ExeFS .code");
    size_t stored_size = (size_t)files[code_index].size;
    *stored = malloc(stored_size);
    *code = calloc(1, (size_t)expected);
    if (!*stored || !*code) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, files[code_index], 0, *stored, stored_size)) return false;
    size_t expanded = stored_size;
    if (exheader[0xd] & 1) {
        if (stored_size < 8 || (uint64_t)stored_size + ctr_le32(*stored + stored_size - 4) > expected)
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "BLZ expansion exceeds segment sizes");
        expanded += ctr_le32(*stored + stored_size - 4);
        if (!snx_ctr_blz(*stored, stored_size, *code, expanded))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ExeFS BLZ stream");
    } else {
        if (stored_size > expected) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "code exceeds process segment sizes");
        memcpy(*code, *stored, stored_size);
    }
    if (expanded < minimum) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "code is shorter than process segments");
    char* path = snx_path_join(stage, "exefs");
    bool ok = ctr_directory(r, path);
    free(path);
    for (size_t i = 0; i < 10 && ok; ++i) {
        if (!*names[i]) continue;
        path = snx_format("%s/exefs/%s", stage, names[i]);
        const unsigned char* file_hash = exefs + 0x1e0 - i * 32;
        if ((int)i == code_index)
            ok = ctr_hash_matches(r, files[i], file_hash) && ctr_write(r, path, *code, (size_t)expected);
        else
            ok = ctr_copy(r, files[i], path, file_hash);
        free(path);
    }
    if (!ok) return false;
    path = snx_path_join(stage, "ncch.bin");
    ok = ctr_write(r, path, h, sizeof h);
    free(path);
    path = snx_path_join(stage, "exheader.bin");
    ok = ok && ctr_write(r, path, exheader, sizeof exheader);
    free(path);
    if (!ok) return false;
    if (rom.size) {
        uint64_t rom_hash_size = (uint64_t)ctr_le32(h + 0x1b8) * 512;
        if (!rom_hash_size || rom_hash_size > rom.size ||
            !ctr_hash_matches(r, (CtrSpan){rom.offset, rom_hash_size}, h + 0x1e0))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS superblock hash/range");
        if (!ctr_romfs_extract(r, rom, stage)) return false;
    }
    if (!rom.size) {
        path = snx_path_join(stage, "romfs");
        ok = ctr_directory(r, path);
        free(path);
        if (!ok) return false;
    }
    char input_hash[65], code_hash[65];
    if (!ctr_sha256(r, *code, (size_t)expected, hash)) return false;
    for (size_t i = 0; i < 32; ++i) {
        snprintf(input_hash + i * 2, 3, "%02x", cia->input_hash[i]);
        snprintf(code_hash + i * 2, 3, "%02x", hash[i]);
    }
    char* manifest =
        snx_format("format=1\nstate=extracted\ntitle_id=%016" PRIx64 "\ntitle_version=%u\ninput_sha256=%s\n"
                   "code_sha256=%s\nentry=0x%08" PRIx32 "\nstack_size=0x%08" PRIx32 "\nbss_size=0x%08" PRIx32 "\n"
                   "text_address=0x%08" PRIx32 "\ntext_pages=%" PRIu32 "\ntext_bytes=%" PRIu32 "\n"
                   "ro_address=0x%08" PRIx32 "\nro_pages=%" PRIu32 "\nro_bytes=%" PRIu32 "\n"
                   "data_address=0x%08" PRIx32 "\ndata_pages=%" PRIu32 "\ndata_bytes=%" PRIu32 "\n",
                   cia->title_id, (unsigned)cia->version, input_hash, code_hash, segments[0].address, stack, bss,
                   segments[0].address, segments[0].pages, segments[0].bytes, segments[1].address, segments[1].pages,
                   segments[1].bytes, segments[2].address, segments[2].pages, segments[2].bytes);
    path = snx_path_join(stage, "manifest.ini");
    ok = ctr_write(r, path, manifest, strlen(manifest));
    free(path);
    free(manifest);
    return ok;
}
bool ctr_ncch_extract(CtrReader* r, const CtrCia* cia, const char* stage) {
    unsigned char *stored = NULL, *code = NULL;
    bool ok = extract_ncch(r, cia, stage, &stored, &code);
    free(stored);
    free(code);
    return ok;
}
