#include <err.h>
#include <stdio.h>

#include "common.h"

int main(int argc, char **argv) {
    int ret;

    if (argc < 2) {
        printf("Usage: %s <file_system.img>\n", argv[0]);
        printf("    reads the root directory in debug mode\n");
        return 1;
    }

    fat_disk_open(argv[1]);

    ret = fatInit();
    if (ret != 0)
        errx(1, "%s", message1);

    fat_debug_readdir(fat_fs.root_cluster);

    return 0;
}
