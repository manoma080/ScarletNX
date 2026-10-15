/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR level-3 layout/hash facts: Azahar core/hle/romfs.cpp and libctru
   romfs_dev.c. Independent serializer over NX's shared host FsTree, not NX
   IPC or Switch RomFS metadata. No original CIA/container reads at runtime. */
#include "romfs.h"
#include "filesystem/tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NONE  UINT32_MAX
#define LIMIT UINT64_C(1073741824)
typedef struct {
    FsNode* node;
    uint32_t parent, next, child, file, off;
} Dir;
typedef struct {
    FsNode* node;
    uint64_t data;
    uint32_t parent, next, off;
} File;
struct SnxCtrRomfs {
    FsTree* tree;
    uint32_t dirs, files;
    size_t meta_size;
    Dir dir[];
};
typedef struct {
    uint32_t dirs, files, dir_bytes, file_bytes;
    uint64_t data;
} Measure;
static File* files(SnxCtrRomfs* r) { return (File*)(r->dir + r->dirs); }
static unsigned char* meta(SnxCtrRomfs* r) { return (unsigned char*)(files(r) + r->files); }
static void put32(unsigned char* b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (i * 8));
}
static void put64(unsigned char* b, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) b[i] = (unsigned char)(v >> (i * 8));
}
static uint64_t align16(uint64_t n) { return (n + 15) & ~UINT64_C(15); }
static uint32_t buckets(uint32_t n) { return n < 3 ? 3 : n | 1; }
static bool name16(const char* s, uint16_t out[256], uint32_t* bytes) {
    unsigned n = 0;
    while (*s) {
        uint32_t cp = (unsigned char)*s++, low = 0;
        unsigned count = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 31;
            count = 1;
            low = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 15;
            count = 2;
            low = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            count = 3;
            low = 0x10000;
        } else if (cp >= 0x80)
            return false;
        for (unsigned i = 0; i < count; ++i) {
            unsigned c = (unsigned char)*s++;
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < low || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp > 0xffff) {
            if (n >= 254) return false;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (cp >> 10));
            out[n++] = (uint16_t)(0xdc00 + (cp & 1023));
        } else {
            if (n >= 255) return false;
            out[n++] = (uint16_t)cp;
        }
    }
    *bytes = n * 2;
    return true;
}
static bool measure(FsNode* node, Measure* m, bool root) {
    uint16_t name[256];
    uint32_t bytes;
    if (!name16(root ? "" : node->name, name, &bytes) || m->dirs + m->files >= 100000) return false;
    if (node->is_dir) {
        ++m->dirs;
        m->dir_bytes += 24 + ((bytes + 3) & ~3u);
        for (FsNode* c = node->children; c; c = c->next)
            if (!measure(c, m, false)) return false;
    } else {
        ++m->files;
        m->file_bytes += 32 + ((bytes + 3) & ~3u);
        uint64_t aligned = align16(m->data);
        if (aligned > LIMIT || node->size > LIMIT - aligned) return false;
        m->data = aligned + node->size;
    }
    return true;
}
static uint32_t hash(uint32_t parent, const uint16_t* name, uint32_t bytes) {
    uint32_t h = parent ^ 123456789u;
    for (uint32_t i = 0; i < bytes / 2; ++i) h = ((h >> 5) | (h << 27)) ^ name[i];
    return h;
}
static void fill(SnxCtrRomfs* r, FsNode* node, uint32_t parent, Measure* m) {
    uint32_t id = m->dirs++;
    Dir* d = &r->dir[id];
    d->node = node;
    d->parent = parent;
    d->off = m->dir_bytes;
    d->next = d->child = d->file = NONE;
    uint16_t name[256];
    uint32_t bytes;
    (void)name16(id ? node->name : "", name, &bytes);
    m->dir_bytes += 24 + ((bytes + 3) & ~3u);
    uint32_t last_dir = NONE, last_file = NONE;
    for (FsNode* c = node->children; c; c = c->next) {
        if (c->is_dir) {
            uint32_t next = m->dirs;
            if (last_dir == NONE)
                d->child = next;
            else
                r->dir[last_dir].next = next;
            last_dir = next;
            fill(r, c, id, m);
        } else {
            uint32_t next = m->files++;
            File* f = &files(r)[next];
            f->node = c;
            f->parent = id;
            f->off = m->file_bytes;
            f->next = NONE;
            if (last_file == NONE)
                d->file = next;
            else
                files(r)[last_file].next = next;
            last_file = next;
            (void)name16(c->name, name, &bytes);
            m->file_bytes += 32 + ((bytes + 3) & ~3u);
            m->data = align16(m->data);
            f->data = r->meta_size + m->data;
            m->data += c->size;
        }
    }
}
static void serialize(SnxCtrRomfs* r, const Measure* m) {
    unsigned char* b = meta(r);
    uint32_t dh = 40, dt = dh + 4 * buckets(r->dirs), fh = dt + m->dir_bytes, ft = fh + 4 * buckets(r->files);
    uint32_t header[] = {40,
                         dh,
                         4 * buckets(r->dirs),
                         dt,
                         m->dir_bytes,
                         fh,
                         4 * buckets(r->files),
                         ft,
                         m->file_bytes,
                         (uint32_t)r->meta_size};
    for (unsigned i = 0; i < 10; ++i) put32(b + i * 4, header[i]);
    memset(b + dh, 0xff, 4 * buckets(r->dirs));
    memset(b + fh, 0xff, 4 * buckets(r->files));
    for (unsigned type = 0; type < 2; ++type) {
        uint32_t count = type ? r->files : r->dirs, ht = type ? fh : dh, table = type ? ft : dt;
        for (uint32_t i = 0; i < count; ++i) {
            Dir* d = type ? NULL : &r->dir[i];
            File* f = type ? &files(r)[i] : NULL;
            uint32_t off = type ? f->off : d->off, parent = r->dir[type ? f->parent : d->parent].off;
            uint16_t name[256];
            uint32_t bytes;
            (void)name16(type ? f->node->name : i ? d->node->name : "", name, &bytes);
            unsigned char* record = b + table + off;
            uint32_t slot = hash(parent, name, bytes) % buckets(count), previous = 0;
            for (unsigned j = 0; j < 4; ++j) previous |= (uint32_t)b[ht + slot * 4 + j] << (j * 8);
            put32(b + ht + slot * 4, off);
            put32(record, parent);
            uint32_t next = type ? f->next : d->next;
            put32(record + 4, next == NONE ? NONE : type ? files(r)[next].off : r->dir[next].off);
            if (type) {
                put64(record + 8, f->data - r->meta_size);
                put64(record + 16, f->node->size);
                put32(record + 24, previous);
                put32(record + 28, bytes);
            } else {
                put32(record + 8, d->child == NONE ? NONE : r->dir[d->child].off);
                put32(record + 12, d->file == NONE ? NONE : files(r)[d->file].off);
                put32(record + 16, previous);
                put32(record + 20, bytes);
            }
            for (uint32_t j = 0; j < bytes / 2; ++j) {
                record[(type ? 32 : 24) + j * 2] = (unsigned char)name[j];
                record[(type ? 32 : 24) + j * 2 + 1] = (unsigned char)(name[j] >> 8);
            }
        }
    }
}
bool snx_ctr_romfs_open(const char* directory, SnxCtrRomfs** out) {
    *out = NULL;
    if (!directory || !*directory) return false;
    FsTree* tree = calloc(1, sizeof *tree);
    if (!tree) return false;
    if (fs_tree_init(tree, directory) != FS_TREE_OK) {
        pm_destroy(&tree->lock);
        free(tree);
        return false;
    }
    pm_lock(&tree->lock);
    FsNode* root = NULL;
    FsTreeResult rc = fs_tree_lookup(tree, NULL, "romfs", &root);
    if (rc == FS_TREE_NOTFOUND) {
        pm_unlock(&tree->lock);
        fs_tree_destroy(tree);
        pm_destroy(&tree->lock);
        free(tree);
        return true;
    }
    Measure m = {0};
    if (rc || !root->is_dir || !measure(root, &m, true)) goto fail;
    uint64_t meta_size = align16(40 + 4ull * buckets(m.dirs) + m.dir_bytes + 4ull * buckets(m.files) + m.file_bytes);
    uint64_t alloc =
        sizeof(SnxCtrRomfs) + (uint64_t)m.dirs * sizeof(Dir) + (uint64_t)m.files * sizeof(File) + meta_size;
    if (meta_size + m.data > LIMIT || alloc > SIZE_MAX) goto fail;
    SnxCtrRomfs* r = calloc(1, (size_t)alloc);
    if (!r) goto fail;
    r->tree = tree;
    r->dirs = m.dirs;
    r->files = m.files;
    r->meta_size = (size_t)meta_size;
    unsigned shared = 0, failed = 0;
    if (fs_tree_share(tree, root, &shared, &failed)) {
        pm_unlock(&tree->lock);
        snx_ctr_romfs_free(r);
        return false;
    }
    Measure pos = {0};
    fill(r, root, 0, &pos);
    serialize(r, &m);
    pm_unlock(&tree->lock);
    *out = r;
    return true;
fail:
    pm_unlock(&tree->lock);
    fs_tree_destroy(tree);
    pm_destroy(&tree->lock);
    free(tree);
    return false;
}
void snx_ctr_romfs_free(SnxCtrRomfs* r) {
    if (r) {
        fs_tree_destroy(r->tree);
        pm_destroy(&r->tree->lock);
        free(r->tree);
        free(r);
    }
}
uint64_t snx_ctr_romfs_size(const SnxCtrRomfs* r) {
    if (!r) return 0;
    pm_lock(&r->tree->lock);
    const File* f = (const File*)(r->dir + r->dirs);
    uint64_t size = r->files ? f[r->files - 1].data + f[r->files - 1].node->size : r->meta_size;
    pm_unlock(&r->tree->lock);
    return size;
}
bool snx_ctr_romfs_read(SnxCtrRomfs* r, uint64_t offset, void* output, size_t size) {
    if (!r || offset > snx_ctr_romfs_size(r) || size > snx_ctr_romfs_size(r) - offset) return false;
    if (!size) return true;
    if (!output) return false;
    pm_lock(&r->tree->lock);
    unsigned char* data = output;
    memset(data, 0, size);
    if (offset < r->meta_size) {
        size_t n = r->meta_size - (size_t)offset;
        if (n > size) n = size;
        memcpy(data, meta(r) + offset, n);
    }
    for (uint32_t i = 0; i < r->files; ++i) {
        File* f = &files(r)[i];
        uint64_t start = offset > f->data ? offset : f->data, end = offset + size;
        if (end > f->data + f->node->size) end = f->data + f->node->size;
        if (start >= end) continue;
        size_t n = (size_t)(end - start);
        uint64_t at = start - f->data;
        unsigned char* dest = data + (size_t)(start - offset);
        FsNode* node = f->node;
        if (node->fd >= 0)
            memcpy(dest, node->map + at, n);
        else {
            FILE* file = fs_tree_open(r->tree, node, "rb");
            if (!file) goto error;
            bool ok = !fseek(file, (long)at, SEEK_SET) && fread(dest, 1, n, file) == n;
            if (fclose(file)) ok = false;
            if (!ok) goto error;
        }
    }
    pm_unlock(&r->tree->lock);
    return true;
error:
    pm_unlock(&r->tree->lock);
    return false;
}
