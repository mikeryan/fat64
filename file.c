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
    int ret, segment, i, num_dirents, total_clusters;
    uint32_t len;
    char long_name[256];
    char short_name[12];
    char segment_chars[26];
    unsigned char crc, *buf;
    uint16_t date_field, time_field;
    uint32_t start_cluster;

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

    // short name is a random alphabetic string
    for (i = 0; i < 11; ++i)
        short_name[i] = 'A' + (rand() % ('Z' - 'A' + 1));
    short_name[11] = 0; // for display purposes

    // printf("Short name %s\n", short_name);

    // calc the CRC
    crc = 0;
    for (i = 0; i < 11; ++i)
        crc = ((crc<<7) | (crc>>1)) + short_name[i];

    // trunc long name to 255 bytes
    memset(long_name, 0, 255);
    strncpy(long_name, filename, 255);
    long_name[255] = '\0';
    len = strlen(long_name);

    num_dirents  = 1; // short filename
    num_dirents += (len - 1) / 13 + 1; // long filename
    // printf("Want to allocate %d dirents\n", num_dirents);

    total_clusters = num_dirents + (dir ? 1 : 0); // allocate dir's first cluster

    // make sure we've got enough room
    if (fat_fs.free_clusters < total_clusters)
        return FAT_NOSPACE;

    ret = fat_allocate_dirents(folder, num_dirents);
    if (ret == FAT_NOSPACE)
        return FAT_INCONSISTENT;

    *result_de = *folder;

    // copy it 13 bytes at a time
    for (segment = len / 13; segment >= 0; --segment) {
        memset(segment_chars, 0, 26);

        // copy up to (and including) the first ASCII NUL
        for (i = 0; i < 13; ++i) {
            segment_chars[2*i] = long_name[segment * 13 + i];
            if (segment_chars[2*i] == '\0') {
                ++i; // make sure we FF after the \0
                break;
            }
        }

        // 0xFF the rest
        for ( ; i < 13; ++i) {
            segment_chars[2*i] = 0xff;
            segment_chars[2*i+1] = 0xff;
        }

        buf = &buffer[folder->index * 32];
        memset(buf, 0, 32);

        // copy the name
        memcpy(&buf[1], &segment_chars[0], 10);
        memcpy(&buf[14], &segment_chars[10], 12);
        memcpy(&buf[28], &segment_chars[22], 4);

        buf[0] = (segment + 1) | ((segment == len / 13) << 6);
        buf[11] = 0x0f;
        buf[13] = crc;

        dir_buffer_dirty = 1;

        // TODO check for inconsistency here
        ++folder->index;
        _fat_load_dir_sector(folder);
    }

    //
    // 8.3 dirent
    //
    buf = &buffer[folder->index * 32];
    memset(buf, 0, 32);

    // copy the short name 
    memcpy(buf, short_name, 11);

    // directory: allocate the first cluster and init it
    if (dir) {
        uint16_t top16, bottom16;

        // attribute: archive and dir flags
        buf[11] = 0x30;

        ret = fat_allocate_cluster(0, &start_cluster);
        if (ret == FAT_NOSPACE)
            return FAT_INCONSISTENT;
        fat_init_dir(start_cluster, folder->first_cluster);

        // flush the newly-allocated cluster
        fat_flush_fat();

        // start cluster
        top16 = (start_cluster >> 16 & 0xffff);
        writeShort(&buf[0x14], top16);

        bottom16 = (start_cluster & 0xffff);
        writeShort(&buf[0x1a], bottom16);
    }

    // regular file
    else {
        // attribute: archive
        buf[11] = 0x20;
    }

    // dates and times
    date_field = (11) | (9 << 5) | (21 << 9); // 9/11/01
    time_field = (46 << 5) | (8 << 11); // 8:46 AM
    writeShort(&buf[14], time_field); // create
    writeShort(&buf[16], date_field); // create
    writeShort(&buf[18], date_field); // access
    writeShort(&buf[22], time_field); // modify
    writeShort(&buf[24], date_field); // modify

    dir_buffer_dirty = 1;
    _fat_flush_dir();

    fat_readdir(result_de);

    /*
    fat_rewind(folder);
    fat_debug_readdir(folder->cluster);
    */

    return FAT_SUCCESS;
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


