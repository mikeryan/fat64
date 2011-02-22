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

static struct fuse_operations fat_oper = {
    .getattr        = getattr,
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
