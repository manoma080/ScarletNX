/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_ROMFS_H
#define SNX_CTR_ROMFS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct SnxCtrRomfs SnxCtrRomfs;
/* Builds CTR level-3 metadata over the same indexed/shared host files as NX.
   Input is an extracted, trusted, non-concurrently-modified title directory.
   Missing romfs/ is genuinely absent (*out stays NULL). No CIA is reopened. */
bool snx_ctr_romfs_open(const char* directory, SnxCtrRomfs** out);
void snx_ctr_romfs_free(SnxCtrRomfs* fs);
uint64_t snx_ctr_romfs_size(const SnxCtrRomfs* fs);
bool snx_ctr_romfs_read(SnxCtrRomfs* fs, uint64_t offset, void* data, size_t size);
/* FSUSER ControlArchive GET_TIMESTAMP over the RomFS view. path16 is the raw
   bounded UTF-16LE request input (NUL-terminated within bytes, <= 12 bytes).
   Returns the backing file's real host modification time as a 3DS datetime:
   milliseconds since 1900-01-01 00:00:00 UTC. */
typedef enum { SNX_CTR_TIME_OK = 0, SNX_CTR_TIME_NOTFOUND, SNX_CTR_TIME_INVALID } SnxCtrTimeResult;
SnxCtrTimeResult snx_ctr_romfs_time(SnxCtrRomfs* fs, const unsigned char* path16, size_t bytes, uint64_t* datetime);
#endif
