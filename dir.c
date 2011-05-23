#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/**
 * Get the root directory entry.
 */
int fat_root_dirent(fat_dirent *dirent) {
    dirent->short_name[0] = 0;
    dirent->long_name[0] = 0;
    dirent->directory = 0;
    dirent->start_cluster = 0;
    dirent->size = 0;

    dirent->index = 0;
    dirent->cluster = fat_fs.root_cluster;
    dirent->sector = 0;
    dirent->first_cluster = fat_fs.root_cluster;

    return 0;
}

/**
 * Get the root directory.
 *
 * Returns:
 *  FAT_SUCCESS always
 */
int fat_root(fat_file_t *file) {
    fat_root_dirent(&file->de);
    file->de.start_cluster = file->de.first_cluster;
    return FAT_SUCCESS;
}

/*
 * Returns a dirent for the directory starting at start_cluster.
 */
void fat_sub_dirent(uint32_t start_cluster, fat_dirent *de) {
    fat_root_dirent(de);
    de->cluster = de->first_cluster = start_cluster;
}

/**
 * Flush pending changes to a directory.
 */
void _fat_flush_dir(void) {
    if (dir_buffer_dirty) {
        cfWriteSector(buffer, dir_buffer_sector);
        dir_buffer_dirty = 0;
    }
}

/**
 * Read a sector from a directory. Automatically handles buffering and flushing
 * changes to dirty buffers.
 */
static void _dir_read_sector(uint32_t sector) {
    if (sector != dir_buffer_sector) {
        // flush pending writes
        _fat_flush_dir();

        cfReadSector(buffer, sector);
        dir_buffer_sector = sector;
    }
}

/**
 * Load the current sector pointed to by the dirent. Automatically loads new
 * sectors and clusters.
 * Returns 0 if next cluster is 0x0ffffff8.
 * Returns 1 on success.
 */
static int _fat_load_dir_sector(fat_dirent *dirent) {
    uint32_t sector;
    uint32_t fat_entry;

    if (dirent->index == DE_PER_SECTOR) {
        // load the next cluster once we reach the end of this
        if (dirent->sector + 1 == fat_fs.sect_per_clus) {
            // look up the cluster number in the FAT
            fat_entry = fat_get_fat(dirent->cluster);
            if (fat_entry >= 0x0ffffff8) // last cluster
                return 0; // end of dir

            dirent->cluster = fat_entry;
            dirent->sector = 0;
            dirent->index = 0;
        }
        else {
            ++dirent->sector;
            dirent->index = 0;
        }
    }

    // sector may or may not have changed, but buffering makes this efficient
    sector = CLUSTER_TO_SECTOR(dirent->cluster) + dirent->sector;
    _dir_read_sector(sector);

    return 1;
}

/**
 * Read a directory.
 * returns:
 *   1  success
 *   0  end of dir
 *  -1  error
 */
int fat_readdir(fat_dirent *dirent) {
    int found_file = 0;
    int ret, i, j;

    uint32_t offset;
    uint32_t attributes;
    uint32_t segment;

    char *dest;

    if (dirent->index > DE_PER_SECTOR) {
        sprintf(message1, "Invalid directory");
        return -1;
    }

    if (dirent->long_name)
        memset(dirent->long_name, 0, 256);

    do {
        ret = _fat_load_dir_sector(dirent);
        if (ret == 0) // end of dir
            return 0;

        offset = dirent->index * 32;

        // end of directory reached
        if (buffer[offset] == 0)
            return 0;

        ++dirent->index;

        // deleted file, skip
        if (buffer[offset] == 0xe5)
            continue;

        attributes = buffer[offset + 0x0b];

        // long filename, copy the bytes and move along
        if (attributes == 0x0f) {
            segment = (buffer[offset] & 0x1F) - 1;
            if (segment < 0 || segment > 19)
                continue; // invalid segment

            dest = (dirent->long_name + segment * 13);

            for (i = 0; i < 5; ++i)
                dest[i] = buffer[offset + 1 + i * 2];

            for (j = 0; j < 3; ++j)
                dest[i+j] = buffer[offset + 0xe + j * 2];

            // last segment can only have 9 characters
            if (segment == 19) {
                dest[i+j] = 0;
                continue;
            }

            for ( ; j < 6; ++j)
                dest[i+j] = buffer[offset + 0xe + j * 2];

            i += j;

            for (j = 0; j < 2; ++j)
                dest[i+j] = buffer[offset + 0x1c + j * 2];

            continue;
        }

        dirent->directory = attributes & 0x10 ? 1 : 0;

        // you can thank FAT16 for this
        dirent->start_cluster = shortEndian(buffer + offset + 0x14) << 16;
        dirent->start_cluster |= shortEndian(buffer + offset + 0x1a);

        dirent->size = intEndian(buffer + offset + 0x1c);

        // copy the name
        memcpy(dirent->short_name, buffer + offset, 8);

        // kill trailing space
        for (i = 8; i > 0 && dirent->short_name[i-1] == ' '; --i)
            ;

        // get the extension
        dirent->short_name[i++] = '.';
        memcpy(dirent->short_name + i, buffer + offset + 8, 3);

        // kill trailing space
        for (j = 3; j > 0 && dirent->short_name[i+j-1] == ' '; --j)
            ;

        // hack! kill the . if there's no extension
        if (j == 0) --j;

        dirent->short_name[i+j] = 0;

        found_file = 1;
    }
    while (!found_file);

    if (dirent->long_name[0] != '\0')
        dirent->name = dirent->long_name;
    else
        dirent->name = dirent->short_name;

    return 1;
}

void fat_rewind(fat_dirent *dirent) {
    // reset the metadata to point to the beginning of the dir
    dirent->index = 0;
    dirent->cluster = dirent->first_cluster;
    dirent->sector = 0;
}

// print a buffer along with a hexdump
static void printbuf(unsigned char *buf, int len) {
    int i;

    printf("'");
    for (i = 0; i < len; ++i)
        printf("%c", isprint(buf[i]) ? buf[i] : '.');
    printf("'  ");
    for (i = 0; i < len; ++i)
        printf("%02x ", buf[i]);
    printf("\n");
}

/**
 * dump a FAT directory to stdout
 */
void fat_debug_readdir(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;

    /*
     * while true:
     *     if cluster = last:
     *         return
     *     load cluster
     *     display entries
     *     get next cluster from fat
     */

    while (1) {
        int index = 0;
        int sector_index = 0;

        printf("Cluster %08x\n", (unsigned int)cluster);

        if (cluster >= 0x0ffffff8) {
            printf("final cluster, done\n");
            return;
        }

        // display the contents of one cluster (may span multiple sectors)
        for (sector_index = 0; sector_index < fat_fs.sect_per_clus; ++sector_index) {
            // load the next sector
            uint32_t sector = CLUSTER_TO_SECTOR(cluster) + sector_index;
            _dir_read_sector(sector);

            for (index = 0; index < 512; index += 32) {
                uint32_t start_cluster;

                printf("  %d/%2d\n", sector_index, index/32);

                if (buffer[index] == 0) {
                    printf("end of dir marker, done\n");
                    return;
                }

                int attributes = buffer[index + 11];
                printf("    %-30s %02x (%s filename)\n", "attributes:", attributes, attributes == 0x0f ? "long" : "short");

                // LFN entry
                if (attributes == 0x0f) {
                    int i, segment;
                    unsigned char buf[13], checksum;

                    segment = (buffer[index] & 0x1F) - 1;
                    printf("    %-30s %d%s\n", "segment: ", segment, (segment < 0 || segment > 19) ? " (invalid)" : "");

                    // copy the bytes of the name
                    for (i = 0; i < 5; ++i)
                        buf[i] = buffer[index + 1 + i * 2];
                    for (i = 0; i < 6; ++i)
                        buf[5+i] = buffer[index + 0xe + i * 2];
                    for (i = 0; i < 2; ++i)
                        buf[11+i] = buffer[index + 0x1c + i * 2];

                    printf("    %-30s ", "bytes:");
                    printbuf(buf, 13);

                    // other info
                    checksum = buffer[index + 13];
                    printf("    %-30s %02x\n", "checksum:", checksum);
                }

                else {
                    int i;
                    unsigned char sum = 0;

                    printf("    %-30s ", "name:");
                    printbuf(buffer + index, 8);

                    printf("    %-30s ", "extension:");
                    printbuf(buffer + index + 8, 3);

                    for (i = 0; i < 11; ++i)
                        sum = (((sum & 1) << 7) | ((sum & 0xfe) >> 1)) + buffer[index + i];
                    printf("    %-30s %02x\n", "checksum (calculated):", sum);
                }

                start_cluster = shortEndian(buffer + index + 0x14) << 16;
                start_cluster |= shortEndian(buffer + index + 0x1a);
                printf("    %-30s %u\n", "start cluster:", (unsigned int)start_cluster);
                if (buffer[index] == 0xe5)
                    printf("    %-30s\n", "deleted");

                printf("        ");
                printbuf(buffer + index, 16);
                printf("        ");
                printbuf(buffer + index + 16, 16);
                printf("    ----\n");
            }
        }

        cluster = fat_get_fat(cluster);
    }
}

/**
 * Fills a cluster with 0's.
 */
static void _fat_clear_cluster(uint32_t cluster) {
    unsigned char clear_buffer[512] = { 0, };
    uint32_t i, sector = CLUSTER_TO_SECTOR(cluster);

    for (i = 0; i < fat_fs.sect_per_clus; ++i)
        cfWriteSector(clear_buffer, sector + i);
}

/**
 * Calculate remaining dirents in a directory at EOD.
 * If the dirent isn't EOD, this value is bogus.
 */
static int _fat_remaining_dirents(fat_dirent *dirent) {
    int remaining;
    uint32_t cluster = dirent->cluster;

    // DE_PER_SECTOR for each unused sector in the cluster
    remaining = DE_PER_SECTOR * (fat_fs.sect_per_clus - (dirent->sector + 1));

    // remaining in the current sector
    remaining += DE_PER_SECTOR - dirent->index;

    // add the rest of the clusters in the directory
    while ((cluster = fat_get_fat(cluster)) < 0x0ffffff7)
        remaining += DE_PER_SECTOR * fat_fs.sect_per_clus;

    return remaining;
}

/**
 * Allocate a bunch of dirents at the end of a directory. If there aren't
 * enough available, allocate a new cluster.
 *
 * Preconditions:
 *  dirent is at end of directory
 *  count >= 0
 *
 * Return:
 *  FAT_SUCCESS     success
 *  FAT_NOSPACE     file system full
 */
int fat_allocate_dirents(fat_dirent *dirent, int count) {
    int remaining, ret;
    uint32_t cluster;

    remaining = _fat_remaining_dirents(dirent);
    if (count > remaining) {
        ret = fat_allocate_cluster(dirent->cluster, &cluster);
        if (ret == FAT_NOSPACE)
            return FAT_NOSPACE;

        fat_flush_fat();
        _fat_clear_cluster(cluster);
    }

    return FAT_SUCCESS;
}

/**
 * Initialize the first cluster of a directory. Creates the . and .. entries.
 */
void fat_init_dir(uint32_t cluster, uint32_t parent) {
    unsigned char buf[512] = { 0, };
    char name[11];
    uint32_t i, sector = CLUSTER_TO_SECTOR(cluster);
    uint16_t top16, bottom16;

    memset(name, 0x20, sizeof(name));

    // FAT32 hack: if parent is root, cluster should be 0
    if (parent == fat_fs.root_cluster)
        parent = 0;

    //
    // .
    //
    name[0] = '.';
    memcpy(&buf[0], name, sizeof(name));
    buf[11] = 0x10; // dir

    // current dir start cluster
    top16 = (cluster >> 16 & 0xffff);
    bottom16 = (cluster & 0xffff);
    writeShort(&buf[0x14], top16);
    writeShort(&buf[0x1a], bottom16);

    //
    // ..
    //
    name[1] = '.';
    memcpy(&buf[32], name, sizeof(name));
    buf[32+11] = 0x10; // dir

    // parent start cluster
    top16 = (parent >> 16 & 0xffff);
    bottom16 = (parent & 0xffff);
    writeShort(&buf[32 + 0x14], top16);
    writeShort(&buf[32 + 0x1a], bottom16);

    //
    // write first sector
    //
    cfWriteSector(buf, sector);

    //
    // zero the rest of the sectors
    //
    memset(buf, 0, 32 * 2);
    for (i = 1; i < fat_fs.sect_per_clus; ++i)
        cfWriteSector(buf, sector + i);
}

/**
 * Write a dirent back to disk.
 */
void _fat_write_dirent(fat_dirent *de) {
    uint32_t sector = CLUSTER_TO_SECTOR(de->cluster) + de->sector;
    uint32_t offset = (de->index - 1) * 32;
    uint16_t top16, bottom16;

    _dir_read_sector(sector);

    // size
    writeInt(&buffer[offset + 0x1c], de->size);

    // start cluster
    top16 = (de->start_cluster >> 16 & 0xffff);
    writeShort(&buffer[offset + 0x14], top16);

    bottom16 = (de->start_cluster & 0xffff);
    writeShort(&buffer[offset + 0x1a], bottom16);

    dir_buffer_dirty = 1;
}

/**
 * Check that there is enough free space to allocate all the dirents for a
 * new file. Also make sure there's space for a new directory's first cluter.
 */
static int _check_free_space(fat_dirent *folder, int num_dirents, int dir) {
    uint32_t total_clusters = 0;

    // if there aren't enough dirents we will allocate a new cluster in the dir
    if (num_dirents > _fat_remaining_dirents(folder))
        ++total_clusters;

    // allocate new dir's first cluster
    if (dir)
        ++total_clusters;

    // make sure we've got enough room
    return fat_fs.free_clusters >= total_clusters;
}

/**
 * Copy a segment of the LFN into its dirent.
 */
static void _copy_lfn_segment(fat_dirent *de, char *long_name, int segment) {
    int i;
    char segment_chars[26];
    unsigned char *buf;

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

    buf = &buffer[de->index * 32];
    memset(buf, 0, 32);

    // copy the name
    memcpy(&buf[1], &segment_chars[0], 10);
    memcpy(&buf[14], &segment_chars[10], 12);
    memcpy(&buf[28], &segment_chars[22], 4);
}

/**
 * Create a file in a directory. The folder dirent must be at EOD. If a dir is
 * created, a cluster will be allocated for it and it will be initialized with
 * . and .. entries.
 *
 * Args:
 *  filename    name of the new file
 *  folder      directory to create it in, MUST be at EOD
 *  result_de   de representing the newly-created file
 *  dir         true if you want to create a dir
 *
 * Returns:
 *  FAT_SUCCESS     success
 *  FAT_NOSPACE     not enough space to create the dirent and/or first cluster of a dir
 *  FAT_INCONSISTENT    fs needs to be checked
 */
int fat_dir_create_file(const char *filename, fat_dirent *folder, fat_dirent *result_de, int dir) {
    int ret, segment, i, num_dirents;
    uint32_t len;
    char long_name[256];
    char short_name[12];
    unsigned char crc, *buf;
    uint16_t date_field, time_field;
    uint32_t start_cluster;

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

    // give up if we don't have enough space
    ret = _check_free_space(folder, num_dirents, dir);
    if (!ret)
        return FAT_NOSPACE;

    ret = fat_allocate_dirents(folder, num_dirents);
    if (ret == FAT_NOSPACE)
        return FAT_INCONSISTENT;

    _fat_load_dir_sector(folder);
    *result_de = *folder;

    // copy it 13 bytes at a time
    for (segment = len / 13; segment >= 0; --segment) {
        _copy_lfn_segment(folder, long_name, segment);

        buf = &buffer[folder->index * 32];

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
