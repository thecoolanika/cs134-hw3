#include "vfs.h"
#include "vnode.h"

/*
 * Look up one name in a directory.
 */
long namev_lookup(vnode_t *dir, const char *name, long namelen,
                  vnode_t **out) {
    if (dir == NULL || name == NULL || out == NULL || namelen <= 0) {
        return -1;
    }
    if (dir->vn_mode != DIRECTORY || dir->vn_ops == NULL ||
        dir->vn_ops->lookup == NULL) { // not a directory
        return -1;
    }
    if (namelen == 1 && name[0] == '.') { // "." is this dir
        *out = dir;
        return 0;
    }
    if (namelen == 2 && name[0] == '.' && name[1] == '.') { // ".." is the parent
        return namev_get_parent(dir, out);
    }
    return dir->vn_ops->lookup(dir, name, namelen, out); // ask the filesystem
}

/*
 * Walk a path like "a/b/c".
 */
long namev_open(vnode_t *base, const char *path, vnode_t **res_vnode) {
    vnode_t *cur;
    const char *p;
    if (path == NULL || res_vnode == NULL) { // bad args
        return -1;
    }
    if (path[0] == '/') { // start at the root
        cur = vfs_root_fs.fs_root;
    } else {
        cur = base;
    }
    if (cur == NULL) {
        return -1;
    }
    p = path;
    while (*p == '/') { // skip extra slashes
        p++;
    }
    if (*p == '\0') {
        *res_vnode = cur;
        return 0;
    }
    while (*p != '\0') {
        const char *start = p;
        long len = 0;
        vnode_t *next;
        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        if (namev_lookup(cur, start, len, &next) != 0) { // name not found
            return -1;
        }
        cur = next;
        p = start + len;
        while (*p == '/') {
            p++;
        }
    }
    *res_vnode = cur;
    return 0;
}

/*
 * Get one child by name.
 */
long namev_get_child(vnode_t *base, const char *name, long namelen,
                     vnode_t **out) {
    if (base == NULL || name == NULL || out == NULL) { // bad args
        return -1;
    }
    if (namelen <= 0) { // empty name
        return -1;
    }
    return namev_lookup(base, name, namelen, out); // same as one lookup
}

/*
 * Get the parent directory.
 */
long namev_get_parent(vnode_t *base, vnode_t **out) {
    if (base == NULL || out == NULL) { // bad args
        return -1;
    }
    if (base == vfs_root_fs.fs_root) { // root's parent is itself
        *out = base;
        return 0;
    }
    if (base->vn_mode != DIRECTORY || base->vn_ops == NULL ||
        base->vn_ops->lookup == NULL) { // only dirs have ".."
        return -1;
    }
    return base->vn_ops->lookup(base, "..", 2, out); // look up ".."
}

/*
 * Check if a is under b.
 */
long namev_is_descendent(vnode_t *a, vnode_t *b) {
    vnode_t *cur;
    long steps;
    if (a == NULL || b == NULL) { // bad args
        return -1;
    }
    cur = a;
    for (steps = 0; steps < 64; steps++) { // walk up from a
        vnode_t *parent;
        if (cur == b) {
            return steps > 0;
        }
        if (cur == vfs_root_fs.fs_root) { // hit the top
            return 0;
        }
        if (namev_get_parent(cur, &parent) != 0) { // can't go up
            return -1;
        }
        if (parent == cur) {
            return 0;
        }
        cur = parent;
    }
    return -1;
}
