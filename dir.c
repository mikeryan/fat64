#include <ctype.h>
#include <stdio.h>
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
int _fat_load_dir_sector(fat_dirent *dirent) {
    uint32_t sector;
    uint32_t fat_entry;

    if (dirent->index == DE_PER_SECTOR) {
        dirent->index = 0;
        ++dirent->sector;

        // load the next cluster once we reach the end of this
        if (dirent->sector == fat_fs.sect_per_clus) {
            // look up the cluster number in the FAT
            fat_entry = fat_get_fat(dirent->cluster);
            if (fat_entry >= 0x0ffffff8) // last cluster
                return 0; // end of dir

            dirent->cluster = fat_entry;
            dirent->sector = 0;
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

        printf("Cluster %08x\n", cluster);

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
                printf("    %-30s %u\n", "start cluster:", start_cluster);
                if (buffer[index] == 0xe5)
                    printf("    %-30s\n", "deleted");

                printbuf(buffer + index, 16);
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
    remaining = DE_PER_SECTOR * fat_fs.sect_per_clus - dirent->sector;

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
