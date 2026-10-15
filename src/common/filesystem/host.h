/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Host-only mutex and shared file primitives, factored from NX's platform
   layer. No guest CPU, kernel, IPC, window or graphics dependency. */
#ifndef SNX_FS_HOST_H
#define SNX_FS_HOST_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef struct {
    SRWLOCK l;
} PMutex;
#define PMUTEX_INIT {SRWLOCK_INIT}
wchar_t* snx_host_wpath(const char* text);
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
} PMutex;
#define PMUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}
#endif
void pm_init(PMutex* m);
void pm_destroy(PMutex* m);
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
int pm_trylock(PMutex* m);
int plat_mkdir(const char* path);
/* Indexed immutable file, opened once and shared with the host page cache.
   Empty files keep a descriptor but no mapping. Failure clears both outputs.
   Keep the input tree trusted and unmodified for the lifetime of its views. */
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data);
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size);
#endif
