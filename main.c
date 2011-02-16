#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

void test_find_create(void) {
    int ret;
    fat_dirent dir, result;

    fat_root_dirent(&dir);
    ret = fat_find_create("abcdefghijklmnopqrstuvqxyz.bin", &dir, &result, 0, 1);

    if (ret == 0)
        printf("result: size %u, start %u\n", result.size, result.start_cluster);
}

void test_set_size(void) {
    int ret;
    fat_dirent dir, result;

    fat_root_dirent(&dir);
    ret = fat_find_create("1", &dir, &result, 0, 0);

    if (ret != 0)
        abort();

    fat_set_size(&dir, 10);
    puts("size 10, hit enter");
    getchar();

    fat_set_size(&dir, 9876);
    puts("size 9876, hit enter");
    getchar();

    fat_set_size(&dir, 5000);
    puts("size 5000, hit enter");
    getchar();

    fat_set_size(&dir, 0);
    puts("size 0");
}

int main(int argc, char **argv) {
    int ret, i;
    uint32_t sectors[10];

    if (argc < 2) {
        printf("Usage: %s <file_system.img>\n", argv[0]);
        return 1;
    }

    srand(time(NULL));

    fat_disk_open(argv[1]);

    ret = fatInit();
    if (ret != 0)
        errx(1, "%s", message1);

    // never leave a loaded gun lying around
    return 0;

    test_find_create();
    return 0;

    /*
    puts("testing set_size");
    test_set_size();
    */

    /*
    fat_debug_readdir(fat_fs.root_cluster);
    return 0;
    */

    puts("testing fat_open");

    fat_file_t root_folder, menu_file, dirrr_dir;

    fat_root(&root_folder);

    ret = fat_open("menuu.bin", &root_folder, NULL, &menu_file);
    // ret = fat_find_create("menu.bin", &root_folder, &menu_file, 0, 0);
    
    printf("result: %s\n", fat_errstr(ret));

    ret = fat_open("dirrr", &root_folder, "cd", &dirrr_dir);
    printf("result: %s\n", fat_errstr(ret));

    ret = fat_open("heya", &dirrr_dir, "", &menu_file);
    printf("result: %s\n", fat_errstr(ret));
    printf("size: %u\n", menu_file.de.size);


    return 0;

    /*
    ret = fatLoadTable();
    if (ret != 0)
        warnx("%s", message1);
    */

    fat_dirent de;
    fat_root_dirent(&de);

    uint32_t start;
    while ((ret = fat_readdir(&de)) > 0) {
        if (strcmp(de.long_name, "menu.bin") == 0) {
            fat_get_sectors(de.start_cluster, sectors, 10);
            start = de.start_cluster;
        }

        printf(
            "%-12s (%c) %5d %s\n",
            de.short_name, de.directory ? 'd' : 'f',
            de.start_cluster,
            de.long_name
        );
    }

    if (ret < 0)
        errx(1, "%s", message1);
 
    printf("sectors for menu.bin: ");
    for (i = 0; i < 10; ++i)
        printf("%d ", sectors[i]);
    printf("\n");

    uint32_t sector, offset;
    ret = fat_get_sector(start, 800, &sector, &offset);
    printf("ret %d, sector %d offset %d\n", ret, sector, offset);
    ret = fat_get_sector(start, 1024, &sector, &offset);
    printf("ret %d, sector %d offset %d\n", ret, sector, offset);
    ret = fat_get_sector(start, 11024, &sector, &offset);
    printf("ret %d, sector %d offset %d\n", ret, sector, offset);
    ret = fat_get_sector(start, 211024, &sector, &offset);
    printf("ret %d, sector %d offset %d\n", ret, sector, offset);

    return 0;
}
