#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "svfs.h"
#include "dirent.h"
#include "slab_allocator.h"
#include "sys/mem/mem.h"

// test 1: read a file from the host and write it into our file system
void test1(vnode_t *root) {
    char buf[1000] = {0};
    char check[1000] = {0};
    vnode_t *file;

    FILE *f = fopen("test/input.txt", "r");
    long len = fread(buf, 1, 1000, f);
    fclose(f);

    if (svfs_lookup(root, "copy.txt", 8, &file) != 0) {
        assert(svfs_mknod(root, "copy.txt", 8, &file) == 0);
    }
    assert(svfs_write(file, 0, buf, len) == len);
    assert(svfs_read(file, 0, check, len) == len);
    assert(memcmp(buf, check, len) == 0);
    printf("test 1 passed\n");
}

// test 2: read a file from our file system and write it onto the host
void test2(vnode_t *root) {
    char buf[1000] = {0};
    char check[1000] = {0};
    vnode_t *file;

    assert(svfs_lookup(root, "copy.txt", 8, &file) == 0);
    long len = svfs_read(file, 0, buf, 1000);
    assert(len > 0);

    FILE *f = fopen("test/host_out.txt", "w");
    fwrite(buf, 1, len, f);
    fclose(f);

    f = fopen("test/host_out.txt", "r");
    assert(fread(check, 1, 1000, f) == len);
    fclose(f);
    assert(memcmp(buf, check, len) == 0);
    printf("test 2 passed\n");
}

// test 3: flush a file to disk, then run again and it should still be there
void test3(vnode_t *root) {
    char buf[100] = {0};
    vnode_t *file;

    if (svfs_lookup(root, "saved.txt", 9, &file) == 0) {
        assert(svfs_read(file, 0, buf, 100) == 5);
        assert(memcmp(buf, "hello", 5) == 0);
        printf("test 3 passed\n");
        return;
    }

    assert(svfs_mknod(root, "saved.txt", 9, &file) == 0);
    assert(svfs_write(file, 0, "hello", 5) == 5);
    mobj_flush(&file->vn_mobj);
    mobj_flush(&root->vn_mobj);
    printf("test 3: saved.txt flushed, run again to check it\n");
}

// test 4: the disk made by fs-maker.py should have a root directory with . and ..
void test4(vnode_t *root) {
    dirent_t *d;

    assert(root->vn_mode == DIRECTORY);
    assert(svfs_readdir(root, 0, &d) == 0);
    assert(strcmp(d->d_name, ".") == 0);
    assert(d->d_ino == 0);
    assert(svfs_readdir(root, sizeof(dirent_t), &d) == 0);
    assert(strcmp(d->d_name, "..") == 0);
    assert(d->d_ino == 0);
    printf("test 4 passed\n");
}

int main() {
    mem_init();
    slab_init();
    svfs_init();
    vnode_t *root = vfs_root_fs.fs_root;
    if (root == NULL) {
        printf("no file system on the disk, run make fs first\n");
        return 1;
    }

    test4(root);
    test1(root);
    test3(root);
    test2(root);
    return 0;
}
