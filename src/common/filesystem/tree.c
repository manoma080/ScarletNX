/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "filesystem/tree.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

/* Bound recursive initialization/deletion without silently omitting a deeper subtree. */
#define FS_TREE_DEPTH_MAX 256

static int separator(char c) { return c == '/' || c == '\\'; }

static FsNode* node_new(const char* name, int is_dir) {
    FsNode* n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->name = strdup(name);
    if (!n->name) {
        free(n);
        return NULL;
    }
    n->is_dir = is_dir;
    n->fd = -1;
    return n;
}

static void node_free(FsNode* n) {
    if (n->fd >= 0) plat_file_unshare(n->fd, n->map, n->size);
    free(n->virtual_data);
    free(n->name);
    free(n);
}

void fs_tree_retain(FsNode* node) {
    if (node) node->refs++;
}

void fs_tree_release(FsNode* node) {
    if (!node || !node->refs) return;
    node->refs--;
    if (!node->refs && !node->attached) node_free(node);
}

static FsNode* child_find(FsNode* parent, const char* name) {
    for (FsNode* n = parent->children; n; n = n->next) {
        int cmp = strcmp(n->name, name);
        if (!cmp) return n;
        if (cmp > 0) break;
    }
    return NULL;
}

static void node_attach(FsNode* parent, FsNode* n) {
    FsNode** p = &parent->children;
    while (*p && strcmp((*p)->name, n->name) < 0) p = &(*p)->next;
    n->next = *p;
    *p = n;
    n->parent = parent;
    n->attached = 1;
}

/* Initial enumeration can contain very large directories. Merge-sort their linked lists once instead of doing
   quadratic sorted insertion for every discovered file. Runtime mutations continue to use sorted insertion. */
static FsNode* nodes_sort(FsNode* head) {
    if (!head || !head->next) return head;
    FsNode *slow = head, *fast = head->next;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    FsNode* right = slow->next;
    slow->next = NULL;
    FsNode *left = nodes_sort(head), *other = nodes_sort(right);
    FsNode *sorted = NULL, **tail = &sorted;
    while (left && other) {
        FsNode** take = strcmp(left->name, other->name) <= 0 ? &left : &other;
        *tail = *take;
        *take = (*take)->next;
        tail = &(*tail)->next;
    }
    *tail = left ? left : other;
    return sorted;
}

/* Unlinking during rename does not release the node. Deletion releases it only after its children are gone. */
static void node_unlink(FsNode* n) {
    FsNode** p = &n->parent->children;
    while (*p != n) p = &(*p)->next;
    *p = n->next;
    n->parent = NULL;
    n->next = NULL;
}

static void node_detach(FsNode* n) {
    if (n->parent) node_unlink(n);
    n->attached = 0;
    if (!n->refs) node_free(n);
}

static void subtree_discard(FsNode* n) {
    while (n->children) subtree_discard(n->children);
    node_detach(n);
}

void fs_tree_destroy(FsTree* tree) {
    if (tree->root) subtree_discard(tree->root);
    tree->root = NULL;
    free(tree->base);
    tree->base = NULL;
}

/* A component must have the same meaning to the guest and the host. In particular, Windows must not interpret
   a guest filename as an alternate stream, device, wildcard, or an alias with ignored trailing dots/spaces. */
static int valid_name(const char* s, size_t len) {
    if (!len || len >= FS_TREE_PATH_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        if (separator(s[i]) || s[i] == ':') return 0;
#if defined(_WIN32)
        if ((unsigned char)s[i] < 32 || strchr("<>\"|?*", s[i])) return 0;
#endif
    }
#if defined(_WIN32)
    if (s[len - 1] == '.' || s[len - 1] == ' ') return 0;
    char stem[16];
    size_t n = 0;
    while (n < len && s[n] != '.' && n < sizeof stem - 1) {
        char c = s[n];
        stem[n++] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    stem[n] = 0;
    if (!strcmp(stem, "CON") || !strcmp(stem, "PRN") || !strcmp(stem, "AUX") || !strcmp(stem, "NUL") ||
        !strcmp(stem, "CONIN$") || !strcmp(stem, "CONOUT$"))
        return 0;
    if ((!strncmp(stem, "COM", 3) || !strncmp(stem, "LPT", 3)) &&
        ((n == 4 && stem[3] >= '1' && stem[3] <= '9') ||
         (n == 5 && (uint8_t)stem[3] == 0xC2 &&
          ((uint8_t)stem[4] == 0xB9 || (uint8_t)stem[4] == 0xB2 || (uint8_t)stem[4] == 0xB3))))
        return 0;
#endif
    return 1;
}

static int belongs_to(FsTree* tree, const FsNode* n) {
    if (!n || !n->attached) return 0;
    while (n->parent) {
        n = n->parent;
        if (!n->attached) return 0;
    }
    return n == tree->root;
}

FsTreeResult fs_tree_path(FsTree* tree, const FsNode* node, char* out, size_t capacity) {
    if (!tree || !tree->base || !out || !capacity) return FS_TREE_INVALID;
    out[0] = 0;
    if (!belongs_to(tree, node)) return FS_TREE_NOTFOUND;
    size_t base_len = strlen(tree->base), size = base_len;
    int base_slash = base_len && separator(tree->base[base_len - 1]);
    for (const FsNode* n = node; n != tree->root; n = n->parent) {
        size_t add = strlen(n->name) + !(n->parent == tree->root && base_slash);
        if (add >= FS_TREE_PATH_MAX || size >= FS_TREE_PATH_MAX - add) return FS_TREE_TOOLONG;
        size += add;
    }
    if (size >= capacity || size >= FS_TREE_PATH_MAX) return FS_TREE_TOOLONG;
    out[size] = 0;
    size_t pos = size;
    for (const FsNode* n = node; n != tree->root; n = n->parent) {
        size_t len = strlen(n->name);
        pos -= len;
        memcpy(out + pos, n->name, len);
        if (n->parent != tree->root || !base_slash) out[--pos] = '/';
    }
    memcpy(out, tree->base, base_len);
    return FS_TREE_OK;
}

/* Normalize before walking the tree: repeated separators and . disappear, and .. may not leave the selected
   mount. No truncated path is ever used for lookup or for a host operation. */
static FsTreeResult parse_path(FsTree* tree, FsNode* scope, const char* path, FsNode** start, char* normalized) {
    if (!tree || !tree->root || !path) return FS_TREE_INVALID;
    size_t path_len = strnlen(path, FS_TREE_PATH_MAX);
    if (path_len >= FS_TREE_PATH_MAX) return FS_TREE_TOOLONG;
    FsNode* base = scope ? scope : tree->root;
    if (!belongs_to(tree, base) || !base->is_dir) return FS_TREE_NOTFOUND;
    const char* p = path;
    if (!scope) {
        const char* colon = strchr(p, ':');
        const char* slash = p;
        while (*slash && !separator(*slash)) slash++;
        if (colon && colon < slash) {
            size_t len = (size_t)(colon - p);
            if (!valid_name(p, len) || (colon[1] && !separator(colon[1]))) return FS_TREE_INVALID;
            char drive[FS_TREE_PATH_MAX];
            memcpy(drive, p, len);
            drive[len] = 0;
            base = child_find(tree->root, drive);
            if (!base || !base->is_dir) return FS_TREE_NOTFOUND;
            p = colon + 1;
        } else if (separator(*p)) {
            base = child_find(tree->root, "root");
            if (!base || !base->is_dir) return FS_TREE_NOTFOUND;
        }
    }
    size_t used = 0;
    while (*p) {
        while (separator(*p)) p++;
        if (!*p) break;
        const char* e = p;
        while (*e && !separator(*e)) e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (!used) return FS_TREE_INVALID;
            while (used && normalized[used - 1] != '/') used--;
            if (used) used--;
        } else {
            if (!valid_name(p, len)) return FS_TREE_INVALID;
            if (used + (used != 0) + len >= FS_TREE_PATH_MAX) return FS_TREE_TOOLONG;
            if (used) normalized[used++] = '/';
            memcpy(normalized + used, p, len);
            used += len;
        }
        p = e;
    }
    normalized[used] = 0;
    *start = base;
    return FS_TREE_OK;
}

static FsTreeResult walk_path(FsNode* start, char* normalized, FsNode** out) {
    FsNode* n = start;
    char* p = normalized;
    while (*p) {
        char* slash = strchr(p, '/');
        if (slash) *slash = 0;
        if (!n->is_dir) return FS_TREE_NOTFOUND;
        n = child_find(n, p);
        if (!n) return FS_TREE_NOTFOUND;
        if (!slash) break;
        p = slash + 1;
    }
    *out = n;
    return FS_TREE_OK;
}

FsTreeResult fs_tree_lookup(FsTree* tree, FsNode* scope, const char* path, FsNode** out) {
    if (!out) return FS_TREE_INVALID;
    *out = NULL;
    char norm[FS_TREE_PATH_MAX];
    FsNode* start;
    FsTreeResult rc = parse_path(tree, scope, path, &start, norm);
    return rc ? rc : walk_path(start, norm, out);
}

/* Return an indexed parent and the final normalized component, whether or not that child exists. */
static FsTreeResult resolve_parent(FsTree* tree, FsNode* scope, const char* path, FsNode** parent, char* name) {
    char norm[FS_TREE_PATH_MAX];
    FsNode* start;
    FsTreeResult rc = parse_path(tree, scope, path, &start, norm);
    if (rc) return rc;
    if (!norm[0]) return FS_TREE_INVALID;
    char* slash = strrchr(norm, '/');
    if (slash) {
        strcpy(name, slash + 1);
        *slash = 0;
        rc = walk_path(start, norm, parent);
        if (rc) return rc;
    } else {
        strcpy(name, norm);
        *parent = start;
    }
    return (*parent)->is_dir ? FS_TREE_OK : FS_TREE_NOTFOUND;
}

static FsTreeResult child_path(FsTree* tree, FsNode* parent, const char* name, char* out) {
    FsTreeResult rc = fs_tree_path(tree, parent, out, FS_TREE_PATH_MAX);
    if (rc) return rc;
    size_t len = strlen(out), add = strlen(name);
    int slash = len && !separator(out[len - 1]);
    if (len + slash + add >= FS_TREE_PATH_MAX) return FS_TREE_TOOLONG;
    if (slash) out[len++] = '/';
    memcpy(out + len, name, add + 1);
    return FS_TREE_OK;
}

static FsTreeResult host_error(void) {
    if (errno == EEXIST) return FS_TREE_EXISTS;
    if (errno == ENOTEMPTY) return FS_TREE_NOTEMPTY;
    if (errno == ENAMETOOLONG) return FS_TREE_TOOLONG;
    return FS_TREE_IO;
}

static unsigned node_depth(const FsNode* n) {
    unsigned depth = 0;
    while (n->parent) {
        depth++;
        n = n->parent;
    }
    return depth;
}

static FsTreeResult materialize(FsTree* tree, FsNode* n) {
    if (!n->is_dir || !belongs_to(tree, n)) return FS_TREE_NOTFOUND;
    FsNode* pending[FS_TREE_DEPTH_MAX];
    unsigned count = 0;
    for (FsNode* p = n; p; p = p->parent) {
        if (p->readonly) return FS_TREE_READONLY;
        if (p->is_virtual) {
            if (count == FS_TREE_DEPTH_MAX) return FS_TREE_TOOLONG;
            pending[count++] = p;
        }
    }
    char path[FS_TREE_PATH_MAX];
    while (count) {
        FsNode* p = pending[--count];
        FsTreeResult rc = fs_tree_path(tree, p, path, sizeof path);
        if (rc) return rc;
        if (plat_mkdir(path)) return FS_TREE_IO;
        p->is_virtual = 0;
    }
    return FS_TREE_OK;
}

static int host_unlink(const char* path, int is_dir) {
#if defined(_WIN32)
    return (is_dir ? RemoveDirectoryA(path) : DeleteFileA(path)) ? 0 : -1;
#else
    return is_dir ? rmdir(path) : unlink(path);
#endif
}

static int host_create_file(const char* path, uint64_t size, int* remains, uint64_t* actual_size) {
    *remains = 0;
    *actual_size = 0;
#if defined(_WIN32)
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
#endif
    if (fd < 0) return -1;
    *remains = 1;
#if defined(_WIN32)
    int rc = size ? (int)_chsize_s(fd, (int64_t)size) : 0;
#else
    int rc = size ? ftruncate(fd, (off_t)size) : 0;
#endif
    if (!rc) *actual_size = size;
    int saved = errno;
    if (rc) {
        /* _chsize_s can write some extension bytes before running out of space. Read the resulting length from
           this already-open handle before rollback; no pathname lookup or host discovery is involved. */
#if defined(_WIN32)
        LARGE_INTEGER actual;
        if (GetFileSizeEx((HANDLE)_get_osfhandle(fd), &actual) && actual.QuadPart >= 0)
            *actual_size = (uint64_t)actual.QuadPart;
#else
        struct stat actual;
        if (!fstat(fd, &actual) && actual.st_size >= 0) *actual_size = (uint64_t)actual.st_size;
#endif
    }
#if defined(_WIN32)
    if (_close(fd) && !rc) {
#else
    if (close(fd) && !rc) {
#endif
        rc = -1;
        saved = errno;
    }
    if (rc && !host_unlink(path, 0)) *remains = 0;
    errno = saved;
    return rc;
}

FsTreeResult fs_tree_create(FsTree* tree, FsNode* scope, const char* path, int is_dir, uint64_t size, FsNode** out) {
    if (out) *out = NULL;
    if (size > INT64_MAX || (is_dir && size)) return FS_TREE_INVALID;
#if !defined(_WIN32)
    if ((off_t)size < 0 || (uint64_t)(off_t)size != size) return FS_TREE_INVALID;
#endif
    FsNode* parent;
    char name[FS_TREE_PATH_MAX], host[FS_TREE_PATH_MAX];
    FsTreeResult rc;
    if (is_dir) {
        /* The normalized mount root already exists, including a virtual root not yet materialized. */
        rc = parse_path(tree, scope, path, &parent, name);
        if (rc) return rc;
        if (!name[0]) return FS_TREE_EXISTS;
    }
    rc = resolve_parent(tree, scope, path, &parent, name);
    if (rc) return rc;
    if (child_find(parent, name)) return FS_TREE_EXISTS;
    if (parent->readonly) return FS_TREE_READONLY;
    if (node_depth(parent) + !!is_dir > FS_TREE_DEPTH_MAX) return FS_TREE_TOOLONG;
    rc = child_path(tree, parent, name, host);
    if (rc) return rc;
    FsNode* n = node_new(name, !!is_dir);
    if (!n) return FS_TREE_NOMEM;
    rc = materialize(tree, parent);
    if (rc) {
        node_free(n);
        return rc;
    }
    if (is_dir) {
        if (plat_mkdir(host)) rc = FS_TREE_IO;
    } else {
        int remains;
        uint64_t actual_size;
        if (host_create_file(host, size, &remains, &actual_size)) {
            rc = host_error();
            /* A failed rollback still left a successfully created host entry; keep that partial result indexed. */
            if (remains) {
                n->size = actual_size;
                node_attach(parent, n);
                return rc;
            }
        }
        n->size = size;
    }
    if (rc) {
        node_free(n);
        return rc;
    }
    node_attach(parent, n);
    if (out) *out = n;
    return FS_TREE_OK;
}

FsTreeResult fs_tree_mkdirs(FsTree* tree, const char* relative_path, FsNode** out) {
    if (out) *out = NULL;
    if (!relative_path || separator(*relative_path) || strchr(relative_path, ':')) return FS_TREE_INVALID;
    char norm[FS_TREE_PATH_MAX];
    FsNode* parent;
    FsTreeResult rc = parse_path(tree, tree ? tree->root : NULL, relative_path, &parent, norm);
    if (rc) return rc;
    char* p = norm;
    while (*p) {
        char* slash = strchr(p, '/');
        if (slash) *slash = 0;
        FsNode* n = child_find(parent, p);
        if (!n) {
            rc = fs_tree_create(tree, parent, p, 1, 0, &n);
            if (rc) return rc;
        }
        if (!n->is_dir) return FS_TREE_NOTFOUND;
        parent = n;
        if (!slash) break;
        p = slash + 1;
    }
    rc = materialize(tree, parent);
    if (!rc && out) *out = parent;
    return rc;
}

static FsTreeResult remove_node(FsTree* tree, FsNode* n, int recursive, int keep, char* host) {
    if (n->readonly) return FS_TREE_READONLY;
    if (n->children && !recursive) return FS_TREE_NOTEMPTY;
    while (n->children) {
        FsTreeResult rc = remove_node(tree, n->children, 1, 0, host);
        if (rc) return rc; /* successful earlier removals are already reflected in the tree */
    }
    if (keep) return FS_TREE_OK;
    if (!n->is_virtual) {
        FsTreeResult rc = fs_tree_path(tree, n, host, FS_TREE_PATH_MAX);
        if (rc) return rc;
        if (host_unlink(host, n->is_dir)) return FS_TREE_IO;
    }
    node_detach(n);
    return FS_TREE_OK;
}

FsTreeResult fs_tree_remove(FsTree* tree, FsNode* scope, const char* path, int is_dir, int recursive, int keep) {
    FsNode* n;
    FsTreeResult rc = fs_tree_lookup(tree, scope, path, &n);
    if (rc) return rc;
    if (n->is_dir != !!is_dir) return FS_TREE_NOTFOUND;
    if (keep && !is_dir) return FS_TREE_INVALID;
    if (!keep && (n == tree->root || n == scope || (n->parent == tree->root && !strcmp(n->name, "root"))))
        return FS_TREE_NOTDELETABLE;
    char host[FS_TREE_PATH_MAX];
    return remove_node(tree, n, recursive, keep, host);
}

static int host_rename(const char* src, const char* dst) {
#if defined(_WIN32)
    return MoveFileA(src, dst) ? 0 : -1;
#else
#if defined(__linux__) && defined(SYS_renameat2)
    /* RENAME_NOREPLACE also protects against an unexpected, unindexed destination without a host probe. */
    if (!syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, 1u)) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    /* Other POSIX hosts have no portable no-replace rename. The index already ruled out a destination and the
       filesystem contract excludes external changes after initialization. */
    return rename(src, dst);
#endif
}

static FsTreeResult subtree_bounds(const FsNode* node, size_t path_len, unsigned depth) {
    if (path_len >= FS_TREE_PATH_MAX || depth > FS_TREE_DEPTH_MAX + !node->is_dir) return FS_TREE_TOOLONG;
    for (FsNode* child = node->children; child; child = child->next) {
        FsTreeResult rc = subtree_bounds(child, path_len + 1 + strlen(child->name), depth + 1);
        if (rc) return rc;
    }
    return FS_TREE_OK;
}

/* Strict ancestry on mount-relative normalized paths. Equal paths and component-prefix siblings are not
   ancestors. Atmosphere's fs::IsSubPath checks both directions, before looking up either directory. */
static int path_ancestor(const char* parent, const char* child) {
    size_t length = strlen(parent);
    if (!length) return *child != 0;
    return !strncmp(parent, child, length) && child[length] == '/';
}

FsTreeResult fs_tree_rename(FsTree* tree, FsNode* scope, const char* src, const char* dst, int is_dir) {
    FsNode *n, *parent;
    char name[FS_TREE_PATH_MAX], from[FS_TREE_PATH_MAX], to[FS_TREE_PATH_MAX];
    FsTreeResult rc;
    if (is_dir) {
        FsNode *from_root, *to_root;
        rc = parse_path(tree, scope, src, &from_root, from);
        if (rc) return rc;
        rc = parse_path(tree, scope, dst, &to_root, to);
        if (rc) return rc;
        if (from_root == to_root && (path_ancestor(from, to) || path_ancestor(to, from))) return FS_TREE_NOTRENAMABLE;
    }
    rc = fs_tree_lookup(tree, scope, src, &n);
    if (rc) return rc;
    if (n->is_dir != !!is_dir) return FS_TREE_NOTFOUND;
    if (n->readonly) return FS_TREE_READONLY;
    if (n == tree->root || n == scope || (n->parent == tree->root && !strcmp(n->name, "root"))) return FS_TREE_INVALID;
    rc = resolve_parent(tree, scope, dst, &parent, name);
    if (rc) return rc;
    if (child_find(parent, name)) return FS_TREE_EXISTS;
    for (FsNode* p = parent; p; p = p->parent)
        if (p == n) return FS_TREE_INVALID;
    if (parent->readonly) return FS_TREE_READONLY;
    rc = fs_tree_path(tree, n, from, sizeof from);
    if (!rc) rc = child_path(tree, parent, name, to);
    if (!rc) rc = subtree_bounds(n, strlen(to), node_depth(parent) + 1);
    if (rc) return rc;
    char* copy = strdup(name);
    if (!copy) return FS_TREE_NOMEM;
    /* Materializing virtual directories is itself a checked indexed mutation. Renaming a physical ancestor also
       moves its virtual descendants in the index, while their file contents continue to live in memory. */
    rc = materialize(tree, parent);
    if (!rc && n->is_virtual) rc = materialize(tree, n);
    if (!rc && host_rename(from, to)) rc = FS_TREE_IO;
    if (rc) {
        free(copy);
        return rc;
    }
    node_unlink(n);
    free(n->name);
    n->name = copy;
    node_attach(parent, n);
    return FS_TREE_OK;
}

FsTreeResult fs_tree_add_virtual(FsTree* tree, const char* relative_path, const void* data, uint64_t size,
                                 FsNode** out) {
    if (out) *out = NULL;
    if (!relative_path || separator(*relative_path) || strchr(relative_path, ':') || size > SIZE_MAX ||
        size > INT64_MAX || (size && !data))
        return FS_TREE_INVALID;
    char norm[FS_TREE_PATH_MAX];
    FsNode* parent;
    FsTreeResult rc = parse_path(tree, tree ? tree->root : NULL, relative_path, &parent, norm);
    if (rc) return rc;
    if (!norm[0]) return FS_TREE_INVALID;
    char* p = norm;
    for (;;) {
        char* slash = strchr(p, '/');
        if (slash) *slash = 0;
        FsNode* n = child_find(parent, p);
        if (!slash && n) return FS_TREE_EXISTS;
        if (!n) {
            char host[FS_TREE_PATH_MAX];
            rc = child_path(tree, parent, p, host);
            if (rc) return rc;
            if (node_depth(parent) + (slash != NULL) > FS_TREE_DEPTH_MAX) return FS_TREE_TOOLONG;
            n = node_new(p, slash != NULL);
            if (!n) return FS_TREE_NOMEM;
            n->is_virtual = 1;
            if (!slash) {
                n->readonly = 1;
                n->size = size;
                if (size) {
                    n->virtual_data = malloc((size_t)size);
                    if (!n->virtual_data) {
                        node_free(n);
                        return FS_TREE_NOMEM;
                    }
                    memcpy(n->virtual_data, data, (size_t)size);
                }
            }
            node_attach(parent, n);
        }
        if (!slash) {
            if (out) *out = n;
            return FS_TREE_OK;
        }
        if (!n->is_dir) return FS_TREE_NOTFOUND;
        parent = n;
        p = slash + 1;
    }
}

FILE* fs_tree_open(FsTree* tree, FsNode* node, const char* mode) {
    char path[FS_TREE_PATH_MAX];
    if (node->is_dir || node->is_virtual || fs_tree_path(tree, node, path, sizeof path)) return NULL;
    FILE* file = fopen(path, mode);
    if (file) setvbuf(file, NULL, _IONBF, 0);
    return file;
}
FsTreeResult fs_tree_share(FsTree* tree, FsNode* node, unsigned* shared, unsigned* failed) {
    if (!belongs_to(tree, node)) return FS_TREE_NOTFOUND;
    node->readonly = 1;
    if (node->is_dir) {
        for (FsNode* child = node->children; child; child = child->next) {
            FsTreeResult rc = fs_tree_share(tree, child, shared, failed);
            if (rc) return rc;
        }
        return FS_TREE_OK;
    }
    if (node->is_virtual || node->fd >= 0) return FS_TREE_OK;
    char path[FS_TREE_PATH_MAX];
    FsTreeResult rc = fs_tree_path(tree, node, path, sizeof path);
    if (rc) return rc;
    if (plat_file_share(path, node->size, &node->fd, &node->map))
        (*failed)++;
    else
        (*shared)++;
    return FS_TREE_OK;
}

/* The initial scan uses native directory APIs because PDir cannot distinguish end-of-directory from failures,
   and its POSIX implementation follows symlinks and skips failed stats. Neither behavior can build a full index. */
static FsTreeResult scan_directory(FsTree* tree, FsNode* parent, char* host, unsigned depth) {
    if (depth > FS_TREE_DEPTH_MAX) return FS_TREE_TOOLONG;
    size_t parent_len = strlen(host);
    FsTreeResult rc = FS_TREE_OK;
#if defined(_WIN32)
    if (parent_len + 3 >= FS_TREE_PATH_MAX) return FS_TREE_TOOLONG;
    strcpy(host + parent_len, "/*");
    WIN32_FIND_DATAA entry;
    HANDLE dir = FindFirstFileA(host, &entry);
    host[parent_len] = 0;
    if (dir == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? FS_TREE_OK : FS_TREE_IO;
    for (;;) {
        const char* name = entry.cFileName;
#else
    DIR* dir = opendir(host);
    if (!dir) return FS_TREE_IO;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            if (errno) rc = FS_TREE_IO;
            break;
        }
        const char* name = entry->d_name;
#endif
        if (strcmp(name, ".") && strcmp(name, "..")) {
            size_t len = strlen(name);
            if (!valid_name(name, len)) {
                rc = FS_TREE_INVALID;
                break;
            }
            if (parent_len + 1 + len >= FS_TREE_PATH_MAX) {
                rc = FS_TREE_TOOLONG;
                break;
            }
            host[parent_len] = '/';
            memcpy(host + parent_len + 1, name, len + 1);
            int is_dir;
            uint64_t size;
#if defined(_WIN32)
            if (entry.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
                rc = FS_TREE_INVALID;
                break;
            }
            is_dir = !!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            size = ((uint64_t)entry.nFileSizeHigh << 32) | entry.nFileSizeLow;
            if (!is_dir) {
                /* Each indexed file has its own size cache. Reject hardlinks so a write through one pathname
                   cannot silently change another node's size. This attribute-only check is startup-only. */
                HANDLE file =
                    CreateFileA(host, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
                if (file == INVALID_HANDLE_VALUE) {
                    rc = FS_TREE_IO;
                    break;
                }
                BY_HANDLE_FILE_INFORMATION info;
                if (!GetFileInformationByHandle(file, &info))
                    rc = FS_TREE_IO;
                else if (info.nNumberOfLinks > 1 ||
                         (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                    rc = FS_TREE_INVALID;
                else
                    size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                if (!CloseHandle(file) && !rc) rc = FS_TREE_IO;
                if (rc) break;
            }
#else
            struct stat st;
            if (lstat(host, &st)) {
                rc = FS_TREE_IO;
                break;
            }
            if ((!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) || st.st_size < 0 ||
                (S_ISREG(st.st_mode) && st.st_nlink > 1)) {
                rc = FS_TREE_INVALID;
                break;
            }
            is_dir = S_ISDIR(st.st_mode);
            size = (uint64_t)st.st_size;
#endif
            FsNode* n = node_new(name, is_dir);
            if (!n) {
                rc = FS_TREE_NOMEM;
                break;
            }
            n->size = is_dir ? 0 : size;
            n->parent = parent;
            n->attached = 1;
            n->next = parent->children;
            parent->children = n;
            if (is_dir) rc = scan_directory(tree, n, host, depth + 1);
            host[parent_len] = 0;
            if (rc) break;
        }
#if defined(_WIN32)
        if (!FindNextFileA(dir, &entry)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) rc = FS_TREE_IO;
            break;
        }
#endif
    }
#if defined(_WIN32)
    if (!FindClose(dir) && !rc) rc = FS_TREE_IO;
#else
    if (closedir(dir) && !rc) rc = FS_TREE_IO;
#endif
    host[parent_len] = 0;
    if (!rc) parent->children = nodes_sort(parent->children);
    return rc;
}

FsTreeResult fs_tree_init(FsTree* tree, const char* directory) {
    if (!tree || !directory || !*directory) return FS_TREE_INVALID;
    memset(tree, 0, sizeof *tree);
    pm_init(&tree->lock);
    char absolute[FS_TREE_PATH_MAX];
#if defined(_WIN32)
    DWORD len = GetFullPathNameA(directory, sizeof absolute, absolute, NULL);
    if (!len) return FS_TREE_IO;
    if (len >= sizeof absolute) return FS_TREE_TOOLONG;
    for (char* p = absolute; *p; p++)
        if (*p == '\\') *p = '/';
    while (len > 3 && absolute[len - 1] == '/') absolute[--len] = 0;
    DWORD attributes = GetFileAttributesA(absolute);
    if (attributes == INVALID_FILE_ATTRIBUTES) return FS_TREE_IO;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FS_TREE_INVALID;
#else
    struct stat st;
    size_t input_len = strnlen(directory, sizeof absolute);
    if (input_len >= sizeof absolute) return FS_TREE_TOOLONG;
    memcpy(absolute, directory, input_len + 1);
    while (input_len > 1 && absolute[input_len - 1] == '/') absolute[--input_len] = 0;
    if (lstat(absolute, &st)) return FS_TREE_IO;
    if (!S_ISDIR(st.st_mode)) return FS_TREE_INVALID;
    char* resolved = realpath(directory, NULL);
    if (!resolved) return FS_TREE_IO;
    size_t len = strlen(resolved);
    if (len >= sizeof absolute) {
        free(resolved);
        return FS_TREE_TOOLONG;
    }
    memcpy(absolute, resolved, len + 1);
    free(resolved);
#endif
    tree->base = strdup(absolute);
    tree->root = node_new("", 1);
    if (!tree->base || !tree->root) {
        fs_tree_destroy(tree);
        return FS_TREE_NOMEM;
    }
    tree->root->attached = 1;
    FsTreeResult rc = scan_directory(tree, tree->root, absolute, 0);
    if (!rc) {
        FsNode* root = child_find(tree->root, "root");
        if (root && !root->is_dir) rc = FS_TREE_INVALID;
        if (!root) {
            root = node_new("root", 1);
            if (!root)
                rc = FS_TREE_NOMEM;
            else {
                root->is_virtual = 1;
                node_attach(tree->root, root);
            }
        }
    }
    if (rc) fs_tree_destroy(tree);
    return rc;
}
