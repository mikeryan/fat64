#define FUSE_USE_VERSION 26

#include <errno.h> // gets the E family of errors, e.g., EIO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuse.h>

#include "fs.h"
#include "common.h"

static int getattr(const char *path, struct stat *stbuf) {
    int ret = 0;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
    }

    else {
        fat_file_t root, file;

        fat_root(&root);

        int fret = fat_open(path + 1, &root, "", &file);
        if (fret == FAT_SUCCESS) {
            // TODO: date
            if (fat_file_isdir(&file)) {
                stbuf->st_mode = S_IFDIR | 0555;
                stbuf->st_size = 0;
                stbuf->st_nlink = 2;
            }
            else {
                stbuf->st_mode = S_IFREG | 0444;
                stbuf->st_size = fat_file_size(&file);
                stbuf->st_nlink = 1;
            }
        }

        else {
            if (fret == FAT_NOTFOUND)
                ret = -ENOENT;

            // other error: something's amiss
            else
                ret = -EIO;
        }
    }

    return ret;
}

static int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    // don't support subdirs yet
    if (strcmp(path, "/") != 0)
        return -EIO;

    fat_dirent root;
    fat_root_dirent(&root);

    while (fat_readdir(&root) > 0)
        filler(buf, root.name, NULL, 0);

    return 0;
}

static int _fat_open(const char *path, struct fuse_file_info *fi) {
    // can't open files in subdirs
    if (strchr(path + 1, '/') != NULL)
        return -EIO;

    // only support read-only mode
    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;

    fat_file_t root, file, *file_save;

    fat_root(&root);
    int ret = fat_open(path + 1, &root, NULL, &file);

    // success: malloc a copy of the file struct and store it in the file_info_t
    if (ret == FAT_SUCCESS) {
        file_save = malloc(sizeof(fat_file_t));
        *file_save = file;
        fi->fh = (uintptr_t)file_save;
        return 0;
    }

    if (ret == FAT_NOTFOUND)
        return -ENOENT;

    // some weird error
    return -EIO;
}

// release is the equivalent of close
// XXX what happens if this gets called multiple times, like with a dup'd fd?
static int _fat_release(const char *path, struct fuse_file_info *fi) {
    free((fat_file_t *)(uintptr_t)fi->fh);
    return 0;
}

static int _fat_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    fat_file_t *file = (fat_file_t *)(uintptr_t)fi->fh;

    // TODO support seeking in the file
    if (offset != file->position)
        return -EIO;

    size = fat_read(file, (unsigned char *)buf, size);

    return size;
}

static struct fuse_operations fat_oper = {
    .getattr        = getattr,
    .readdir        = readdir,
    .open           = _fat_open,
    .release        = _fat_release,
    .read           = _fat_read,
};

int main(int argc, char **argv) {
    fat_disk_open("test/fat32.fs");
    int ret = fat_init();
    if (ret != 0) {
        puts(message1);
        abort();
    }

    return fuse_main(argc, argv, &fat_oper, NULL);
}
