/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#include "filesystem/host.h"
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
void pm_init(PMutex* m) { InitializeSRWLock(&m->l); }
void pm_destroy(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m) { ReleaseSRWLockExclusive(&m->l); }
int pm_trylock(PMutex* m) { return TryAcquireSRWLockExclusive(&m->l) != 0; }
wchar_t* snx_host_wpath(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = malloc((size_t)n * sizeof *w);
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        w = NULL;
    }
    return w;
}
/* The CRT's descriptor table holds 8192 (_NHANDLE_), and descriptors are handed out lowest first: leave room for
   everything opened on demand */
#define SHARE_FD_MAX     8192
#define SHARE_FD_RESERVE 256
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    *fd = -1;
    *data = NULL;
    if (size > SIZE_MAX || size > INT64_MAX) return -1;
    wchar_t* w = snx_host_wpath(path);
    if (!w) return -1;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE) return -1;
    void* p = NULL;
    if (size) { /* a section is shared by every process that maps the file, and reads a page when it is touched */
        HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, (DWORD)(size >> 32), (DWORD)size, NULL);
        p = mp ? MapViewOfFile(mp, FILE_MAP_READ, 0, 0, (SIZE_T)size) : NULL;
        if (mp) CloseHandle(mp); /* the view keeps the mapping */
        if (!p) {
            CloseHandle(f);
            return -1;
        }
    }
    int d = _open_osfhandle((intptr_t)f, _O_RDONLY | _O_BINARY); /* owns f from here */
    if (d < 0 || d + SHARE_FD_RESERVE >= SHARE_FD_MAX) {
        if (p) UnmapViewOfFile(p);
        if (d < 0)
            CloseHandle(f);
        else
            _close(d);
        return -1;
    }
    *fd = d;
    *data = p;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    (void)size;
    if (data) UnmapViewOfFile(data);
    if (fd >= 0) _close(fd);
}
int plat_mkdir(const char* path) { return CreateDirectoryA(path, NULL) ? 0 : -1; }
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_destroy(PMutex* m) { pthread_mutex_destroy(&m->l); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
int pm_trylock(PMutex* m) { return !pthread_mutex_trylock(&m->l); }
/* Descriptors left to everything opened on demand: saves, the Vulkan driver, audio, dlopen */
#define SHARE_FD_RESERVE 256
/* A RomFS holds thousands of files and the soft limit is usually 1024, against a hard one in the hundreds of
   thousands: raise it once. Nothing in the runtime uses select(), the one API that breaks on descriptors >= 1024 */
static rlim_t share_fd_limit(void) {
    static rlim_t limit;
    if (!limit) {
        struct rlimit r;
        if (getrlimit(RLIMIT_NOFILE, &r)) {
            limit = 1024;
        } else {
            if (r.rlim_cur < r.rlim_max) {
                rlim_t soft = r.rlim_cur;
                r.rlim_cur = r.rlim_max;
                if (setrlimit(RLIMIT_NOFILE, &r)) r.rlim_cur = soft; /* a hard limit above the kernel's nr_open */
            }
            limit = r.rlim_cur;
        }
    }
    return limit;
}
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    *fd = -1;
    *data = NULL;
    if (size > SIZE_MAX || size > INT64_MAX) return -1;
    rlim_t limit = share_fd_limit();
    int f = open(path, O_RDONLY | O_CLOEXEC);
    if (f < 0) return -1;
    /* descriptors are handed out lowest first, so the number counts the ones in use */
    if (limit != RLIM_INFINITY && (rlim_t)f + SHARE_FD_RESERVE >= limit) {
        close(f);
        return -1;
    }
    if (size) { /* lazy: no MAP_POPULATE, a page is read when the game first touches it */
        void* p = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, f, 0);
        if (p == MAP_FAILED) {
            close(f);
            return -1;
        }
        *data = p;
    }
    *fd = f;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    if (data) munmap((void*)data, (size_t)size);
    if (fd >= 0) close(fd);
}
int plat_mkdir(const char* path) { return mkdir(path, 0755); }
#endif
