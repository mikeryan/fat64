#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/**
 * Find a file by name in a directory. Create it if it doesn't exist. Use the
 * dir flag to specify that you want to create a directory.
 *
 * Return:
 *   FAT_SUCCESS    success
 *   FAT_NOTFOUND   file/dir not found and create flag not set
 *   FAT_NOSPACE    file system full
 *   FAT_INCONSISTENT fs needs to be checked
 */
int fat_find_create(char *filename, fat_dirent *folder, fat_dirent *result_de, int dir, int create) {
    int ret;

    //
    // Try to find the file in the dir, return it if found
    //

    while ((ret = fat_readdir(folder)) > 0)
        if (strcasecmp(filename, folder->name) == 0) {
            // found, return it
            *result_de = *folder;
            return FAT_SUCCESS;
        }

    //
    // File not found. Create a new file
    //

    if (!create)
        return FAT_NOTFOUND;

    return fat_dir_create_file(filename, folder, result_de, dir);
}

/**
 * TODO desc
 *
 * flags:
 *      c   create the file/directory if it doesn't exist
 *      d   used with c, creates a directory instead of a file
 *
 * flags MAY be NULL, in which case no flags are assumed
 *
 * Return:
 *   FAT_SUCCESS    success
 *   FAT_NOTFOUND   file/dir not found and create flag not set
 *   FAT_NOSPACE    file system full
 *   FAT_INCONSISTENT fs needs to be checked
 */
int fat_open(char *filename, fat_file_t *folder, char *flags, fat_file_t *file) {
    int dir, create, ret;
    fat_dirent folder_de, result_de;

    if (flags == NULL) flags = "";
    dir = strchr(flags, 'd') != NULL;
    create = strchr(flags, 'c') != NULL;

    fat_sub_dirent(folder->de.start_cluster, &folder_de);

    ret = fat_find_create(filename, &folder_de, &result_de, dir, create);
    if (ret != FAT_SUCCESS)
        return ret;

    file->de = result_de;
    file->de.first_cluster = result_de.start_cluster;
    file->de.index = 0;
    file->de.cluster = result_de.start_cluster;
    file->de.sector = 0;

    file->position = 0;

    return FAT_SUCCESS;
}

/**
 * Sets file size, adding and removing clusters as necessary.
 * Returns:
 *  FAT_SUCCESS on success
 *  FAT_NOSPACE if the file system is full
 *  FAT_INCONSISTENT if the file system needs to be checked
 */
int fat_set_size(fat_dirent *de, uint32_t size) {
    int ret;
    uint32_t bytes_per_clus = fat_fs.sect_per_clus * 512;
    uint32_t current_clusters, new_clusters;

    // true NOP, no change in size whatsoever
    if (de->size == size)
        return 0;

    current_clusters = ceil(1.0 * de->size / bytes_per_clus);
    new_clusters = ceil(1.0 * size / bytes_per_clus);

    // expand file
    if (new_clusters > current_clusters) {
        uint32_t count = 0;
        uint32_t current = de->start_cluster;

        // first make sure we have enough clusters
        if (new_clusters - current_clusters > fat_fs.free_clusters)
            return FAT_NOSPACE;

        // if the file's empty it will have no clusters, so create the first
        if (current == 0) {
            ret = fat_allocate_cluster(0, &current);

            // see comment below about FAT_INCONSISTENT for details
            if (ret == FAT_NOSPACE)
                return FAT_INCONSISTENT;

            de->start_cluster = current;
            ++count;
        }

        // otherwise skip to last entry
        else {
            uint32_t prev = current;
            while ((current = fat_get_fat(current)) < 0x0ffffff8) {
                ++count;
                prev = current;
            }
            current = prev;
            ++count; // account for terminating sector
        }

        // add new clusters
        while (count < new_clusters) {
            ret = fat_allocate_cluster(current, &current);

            // we already made sure there would be enough clusters according
            // to metadata, so if we can't allocate a cluster then the file
            // system must be inconsistent
            if (ret == FAT_NOSPACE)
                return FAT_INCONSISTENT;

            ++count;
        }
    }

    // remove sectors
    else if (new_clusters < current_clusters) {
        uint32_t count;
        uint32_t current = de->start_cluster;
        uint32_t prev = current;

        // skip to the first entry past the new last FAT entry
        for (count = 0; count < new_clusters; ++count) {
            prev = current;
            current = fat_get_fat(current);
        }

        // if the file's not empty, set the last FAT entry to END
        if (count > 0)
            fat_set_fat(prev, 0x0ffffff8);
        else
            de->start_cluster = 0;

        // zero the rest of the FAT entries
        do {
            uint32_t next = fat_get_fat(current);
            fat_set_fat(current, 0);
            current = next;
            ++fat_fs.free_clusters;
        } while (current < 0x0ffffff6 && current > 0);
    }

    else // (new_clusters == current_clusters), NOP but still need to update dirent
        ;

    // update size in dirent
    de->size = size;

    // write it back to disk
    _fat_write_dirent(de);

    fat_flush_fat();
    _fat_flush_dir();

    return 0;
}


