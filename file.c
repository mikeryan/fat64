#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

unsigned char file_buffer[512];
uint32_t file_buffer_sector = 0;

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
int fat_find_create(const char *filename, fat_dirent *folder, fat_dirent *result_de, int dir, int create) {
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
 * open a file a la fopen
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
int fat_open(const char *filename, fat_file_t *folder, char *flags, fat_file_t *file) {
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

    file->cluster = result_de.start_cluster;
    file->sector = 0;
    file->offset = 0;
    file->position = 0;

    return FAT_SUCCESS;
}

// load the current sector from the file
// cut & paste code from _dir_load_sector :(
//
// returns:
//  FAT_SUCCESS         success
//  FAT_INCONSISTENT    fs inconsistent
static int _fat_load_file_sector(fat_file_t *file) {
    uint32_t sector;
    uint32_t fat_entry;

    if (file->offset == 512) {
        if (file->sector + 1 == fat_fs.sect_per_clus) {
            // look up the cluster number in the FAT
            fat_entry = fat_get_fat(file->cluster);
            if (fat_entry >= 0x0ffffff8) // last cluster
                return FAT_INCONSISTENT;

            file->cluster = fat_entry;
            file->sector = 0;
            file->offset = 0;
        }
        else {
            ++file->sector;
            file->offset = 0;
        }
    }

    // sector may or may not have changed, but buffering makes this efficient
    sector = CLUSTER_TO_SECTOR(file->cluster) + file->sector;

    // TODO dirty file cluster?
    if (file_buffer_sector != sector) {
        cfReadSector(file_buffer, sector);
        file_buffer_sector = sector;
    }

    return FAT_SUCCESS;
}

/**
 * Read len bytes out of file into buf.
 *
 * Returns the number of bytes read.
 * If return value == 0, file is at EOF.
 *
 * Return of -1 indicates error.
 */
int32_t fat_read(fat_file_t *file, unsigned char *buf, int32_t len) {
    uint32_t bytes_read = 0;
    int ret;

    if (len < 0)
        return -1;

    if (file->position + len > file->de.size)
        len = file->de.size - file->position;

    while (bytes_read < len) {
        int bytes_left;

        ret = _fat_load_file_sector(file);
        // FIXME: fs_error() function
        if (ret != FAT_SUCCESS)
            return -1;

        bytes_left = 512 - file->offset;
        if (bytes_read + bytes_left > len)
            bytes_left = len - bytes_read;

        memcpy(buf + bytes_read, file_buffer + file->offset, bytes_left);
        bytes_read += bytes_left;
        file->position += bytes_left;

        file->offset += bytes_left;
    }

    return bytes_read;
}

/**
 * Seek to absolute position in file.
 * Returns:
 *  FAT_SUCCESS on success
 *  FAT_INCONSISTENT if the file system needs to be checked
 */
int fat_seek(fat_file_t *file, uint32_t position) {
    uint32_t bytes_per_clus = fat_fs.sect_per_clus * 512;
    uint32_t seek_left = position;

    // trunc position
    if (position > file->de.size) position = file->de.size;

    file->sector = 0;

    uint32_t cluster = file->de.start_cluster;

    while (seek_left > 512) {
        cluster = fat_get_fat(cluster);
        if (cluster >= 0x0ffffff8)
            return FAT_INCONSISTENT;

        // found the right cluster, so find the right sector and offset
        if (seek_left < bytes_per_clus) {
            file->cluster = cluster;
            while (seek_left >= 512) {
                seek_left -= 512;
                ++file->sector;
            }
        }

        // more clusters to go
        else
            seek_left -= bytes_per_clus;
    }

    file->cluster = cluster;
    file->offset = seek_left;
    file->position = position;
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

/**
 * Accessor: returns true if a file is a dir
 */
int fat_file_isdir(fat_file_t *file) {
    return file->de.directory ? 1 : 0;
}

/**
 * Accessor: returns a file's size
 */
uint32_t fat_file_size(fat_file_t *file) {
    return file->de.size;
}
