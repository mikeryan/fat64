#ifdef LINUX

#include <ctype.h>
#include <err.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;

// the disk image
FILE *cf_file;

typedef struct _fat_dirent {
    // file properties
    char *name;
    char short_name[13];
    char long_name[256];
    int directory;
    uint32_t start_cluster;
    uint32_t size;

    // metadata
    uint32_t index;
    uint32_t cluster;
    uint32_t sector;

    uint32_t first_cluster;
} fat_dirent;

unsigned char buffer[512];
char message1[BUFSIZ];

unsigned char fat_buffer[512];
unsigned long fat_buffer_sector = -1;
int fat_buffer_dirty = 0;

uint32_t dir_buffer_sector = 0;
int dir_buffer_dirty = 0;

uint32_t fs_begin_sector;

typedef struct _fat_fs_t {
    uint32_t begin_sector;
    uint32_t sect_per_clus;
    uint32_t num_fats;
    uint32_t sect_per_fat;
    uint32_t root_cluster;
    uint32_t clus_begin_sector;
    uint32_t total_clusters;
    uint32_t free_clusters;
} fat_fs_t;

fat_fs_t fat_fs;

int             compat_mode = 0;

void loadRomToRam(uint32_t ramaddr, uint32_t clus);

void cfSectorToRam(uint32_t ramaddr, uint32_t lba);
void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors);
void cfReadSector(unsigned char *buffer, uint32_t lba);
void cfWriteSector(unsigned char *buffer, uint32_t lba);

void cfSetCycleTime(int cycletime);
int cfOptimizeCycleTime();

void fat_sector_offset(uint32_t cluster, uint32_t *fat_sector, uint32_t *fat_offset);
uint32_t fat_get_fat(uint32_t cluster);

uint32_t intEndian(unsigned char *i);
void writeInt(unsigned char *dest, uint32_t val);
unsigned short shortEndian(unsigned char *i);
void writeShort(unsigned char *dest, uint16_t val);

int fat_root_dirent(fat_dirent *dirent);

#endif

// 2-byte number
unsigned short shortEndian(unsigned char *i)
{
// assume little endian system for debug
#ifdef LINUX
    return *(unsigned short *)i;
#else

    int t = ((int)*(i+1)<<8) |
            ((int)*(i+0));
    return t;
#endif
}

void writeShort(unsigned char *dest, uint16_t val) {
    *(uint16_t *)dest = shortEndian((unsigned char *)&val);
}

// 4-byte number
unsigned int intEndian(unsigned char *i)
{
#ifdef LINUX
    return *(uint32_t *)i;
#else
    uint32_t t = ((int)*(i+3)<<24) |
            ((int)*(i+2)<<16) |
            ((int)*(i+1)<<8) |
            ((int)*(i+0));

    return t;
#endif
}

void writeInt(unsigned char *dest, uint32_t val) {
    *(uint32_t *)dest = intEndian((unsigned char *)&val);
}

/************************
 * CONSTANTS AND MACROS *
 ************************/

// first sector of a cluster
#define CLUSTER_TO_SECTOR(X) ( fat_fs.clus_begin_sector + (X - 2) * fat_fs.sect_per_clus )

// dirents per sector
#define DE_PER_SECTOR (512 / 32)

#define FAT_SUCCESS 0
#define FAT_NOSPACE 1
#define FAT_INCONSISTENT 256

int fatInit()
{
    char fat_systemid[8];
    uint32_t fat_num_resv_sect;
    uint32_t total_sectors, data_offset;

    // read first sector
    cfReadSector(buffer, 0);

    // check for MBR/VBR magic
    if( !(buffer[0x1fe]==0x55 && buffer[0x1ff]==0xAA) ){
        sprintf(message1, "No CF card / bad filesystem");
        return 1;
    }

#ifndef LINUX
    // now speed things up!
    if(compat_mode)
    {
        cfSetCycleTime(30);
    }else{
        cfOptimizeCycleTime();
    }
#endif

    // look for 'FAT'
    if(strncmp((char *)&buffer[82], "FAT", 3) == 0)
    {
        // this first sector is a Volume Boot Record
        fs_begin_sector = 0;
    }else{
        // this is a MBR. Read first entry from partition table
        fs_begin_sector = intEndian(&buffer[0x1c6]);
    }

    cfReadSector(buffer, fs_begin_sector);

    // copy the system ID string
    memcpy(fat_systemid, &buffer[82], 8);
    fat_systemid[8] = 0;

    if(strncmp(fat_systemid, "FAT32", 5) != 0){
        // not a fat32 volume
        sprintf(message1, "FAT32 partition not found.");
        return 1;
    }

    fat_fs.sect_per_clus = buffer[0x0d];
    fat_num_resv_sect = shortEndian(&buffer[0x0e]);
    fat_fs.num_fats = buffer[0x10];
    fat_fs.sect_per_fat = intEndian(&buffer[0x24]);
    fat_fs.root_cluster = intEndian(&buffer[0x2c]);

    fat_fs.begin_sector = fs_begin_sector + fat_num_resv_sect;
    data_offset = fat_num_resv_sect + (fat_fs.num_fats * fat_fs.sect_per_fat);
    fat_fs.clus_begin_sector = fs_begin_sector + data_offset;

    total_sectors = intEndian(&buffer[0x20]);
    fat_fs.total_clusters = (total_sectors - data_offset) / fat_fs.sect_per_clus;

    //
    // Load free cluster count
    //
    cfReadSector(buffer, fs_begin_sector + 1);
    fat_fs.free_clusters = intEndian(&buffer[0x1e8]);

    sprintf(message1, "Loaded successfully.");

    return 0;
}

/**
 * Get the relative sector # and offset into the sector for a given cluster.
 */
void fat_sector_offset(uint32_t cluster, uint32_t *fat_sector, uint32_t *fat_offset) {
    uint32_t index = cluster * 4;   // each cluster is 4 bytes long
    uint32_t sector = index / 512;  // 512 bytes per sector, rounds down

    *fat_sector = sector;
    *fat_offset = index - sector * 512;
}

/**
 * Get the absolute number of a relative fat sector for a given fat.
 */
static uint32_t _fat_absolute_sector(uint32_t relative_sector, int fat_num) {
    return fat_fs.begin_sector + (fat_num * fat_fs.sect_per_fat) + relative_sector;
}

// flush changes to the fat
static void _fat_flush_fat(void) {
    uint32_t sector, i;
    unsigned char fs_info[512];
    uint32_t old_free;

    if (fat_buffer_dirty) {
        // write the dirty sector to each copy of the FAT
        for (i = 0; i < fat_fs.num_fats; ++i) {
            sector = _fat_absolute_sector(fat_buffer_sector, i);
            cfWriteSector(fat_buffer, sector);
        }

        fat_buffer_dirty = 0;
    }

    // 
    // Write free cluster count
    //
    cfReadSector(fs_info, fs_begin_sector + 1);
    old_free = intEndian(&fs_info[0x1e8]);
    if (old_free != fat_fs.free_clusters) {
        writeInt(&fs_info[0x1e8], fat_fs.free_clusters);
        cfWriteSector(fs_info, fs_begin_sector + 1);
    }
}

/**
 * Load the sector for a FAT cluster, return offset into sector.
 */
uint32_t _fat_load_fat(uint32_t cluster) {
    uint32_t relative_sector, offset;

    // get the sector of the FAT and offset into the sector
    fat_sector_offset(cluster, &relative_sector, &offset);

    // only read sector if it has changed! saves time
    if (relative_sector != fat_buffer_sector) {
        // flush pending writes
        _fat_flush_fat();

        // read the sector
        cfReadSector(fat_buffer, _fat_absolute_sector(relative_sector, 0));
        fat_buffer_sector = relative_sector;
    }

    return offset;
}


/**
 * Get the FAT entry for a given cluster.
 */
uint32_t fat_get_fat(uint32_t cluster) {
    uint32_t offset = _fat_load_fat(cluster);
    return intEndian(&fat_buffer[offset]);
}

/**
 * Set the FAT entry for a given cluster.
 */
void fat_set_fat(uint32_t cluster, uint32_t value) {
    uint32_t offset = _fat_load_fat(cluster);
    writeInt(&fat_buffer[offset], value);

    fat_buffer_dirty = 1;
}

/**
 * Find the first unused entry in the FAT.
 *
 * Returns:
 *  FAT_SUCCESS on success, with new entry in new_entry
 *  FAT_NOSPACE if it can't find an unused cluster
 */
static int _fat_find_free_entry(int start, uint32_t *new_entry) {
    uint32_t entry = 1;
    uint32_t num_entries = fat_fs.total_clusters + 2; // 2 unused entries at the start of the FAT

    if (start > 0)
        entry = start;

    while (entry < num_entries && fat_get_fat(entry) != 0)
        ++entry;

    // if we reach the end, loop back to the beginning and try to find an unused entry
    if (entry == num_entries) {
        entry = 1;
        while (entry < start && fat_get_fat(entry) != 0)
            ++entry;
        if (entry == start)
            return FAT_NOSPACE;
    }

    *new_entry = entry;
    return FAT_SUCCESS;
}

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
static void _fat_flush_dir(void) {
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

                printbuf(buffer + index, 16);
                printbuf(buffer + index + 16, 16);
                printf("    ----\n");
            }
        }

        cluster = fat_get_fat(cluster);
    }
}

/**
 * Get an array of all the sectors a file owns. Returns all sectors of all
 * clusters even if the file does not occupy all the sectors (i.e., a 512 byte
 * file will still return 4 sectors in a 4 sector-per-cluster FS).
 *
 * Return 0 on succes, -1 on fail
 * Only failure mode is if the size of the sectors array is too small.
 */
int fat_get_sectors(uint32_t start_cluster, uint32_t *sectors, int size) {
    int i, num = 0;
    uint32_t cluster, sector;

    cluster = start_cluster;

    while (cluster < 0x0ffffff8) {
        sector = CLUSTER_TO_SECTOR(cluster);
        for (i = 0; i < fat_fs.sect_per_clus; ++i) {
            // make sure we have enough space for the sectors
            if (num >= size)
                return -1;

            sectors[num++] = sector + i;
        }
        cluster = fat_get_fat(cluster);
    }

    return 0;
}

/**
 * Allocate a new cluster after the last cluster. Sets the end of file marker
 * in the FAT as a bonus.
 * 
 * If last_cluster is 0, it assumes this is the first cluster in the file and
 * WILL NOT set the FAT entry on cluster 0.
 * 
 * Returns:
 *  FAT_SUCCESS on success, new cluster in new_cluster
 *  FAT_NOSPACE when the FS has no free clusters
 *  FAT_INCONSISTENT when the fs needs to be checked
 */
static int _fat_allocate_cluster(uint32_t last_cluster, uint32_t *new_cluster) {
    int ret;
    uint32_t new_last;

    if (fat_fs.free_clusters == 0)
        return FAT_NOSPACE;

    ret = _fat_find_free_entry(last_cluster, &new_last);

    // according to the free cluster count, we should be able to find a free
    // cluster. since _fat_find_free_entry couldn't, this means the FS must be
    // in an inconistent state
    if (ret == FAT_NOSPACE)
        return FAT_INCONSISTENT;

    // see comment above for explanation
    if (last_cluster != 0)
        fat_set_fat(last_cluster, new_last);

    fat_set_fat(new_last, 0x0ffffff8);
    --fat_fs.free_clusters;

    *new_cluster = new_last;
    return FAT_SUCCESS;
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
    while ((cluster = fat_get_fat(cluster)) < 0xfffff7)
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
static int _fat_allocate_dirents(fat_dirent *dirent, int count) {
    int remaining, ret;
    uint32_t cluster;

    remaining = _fat_remaining_dirents(dirent);
    if (count > remaining) {
        ret = _fat_allocate_cluster(dirent->cluster, &cluster);
        if (ret == FAT_NOSPACE)
            return FAT_NOSPACE;

        _fat_flush_fat();
        _fat_clear_cluster(cluster);
    }

    return FAT_SUCCESS;
}

/**
 * Initialize the first cluster of a directory. Creates the . and .. entries.
 */
static void _fat_init_dir(uint32_t cluster, uint32_t parent) {
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
 * Find a file by name in a directory. Create it if it doesn't exist. Use the
 * dir flag to specify that you want to create a directory.
 *
 * Return:
 *   FAT_SUCCESS    success
 *   FAT_NOSPACE    file system full
 *   FAT_INCONSISTENT fs needs to be checked
 */
int fat_find_create(char *filename, fat_dirent *folder, fat_dirent *result_de, int dir) {
    int ret, segment, i, num_dirents, total_clusters;
    size_t len;
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

    ret = _fat_allocate_dirents(folder, num_dirents);
    ret = FAT_SUCCESS;
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

        ret = _fat_allocate_cluster(0, &start_cluster);
        if (ret == FAT_NOSPACE)
            return FAT_INCONSISTENT;
        _fat_init_dir(start_cluster, folder->first_cluster);

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
 * Write a dirent back to disk.
 */
static void _fat_write_dirent(fat_dirent *de) {
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
            ret = _fat_allocate_cluster(0, &current);

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
            ret = _fat_allocate_cluster(current, &current);

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

    _fat_flush_fat();
    _fat_flush_dir();

    return 0;
}

#ifdef LINUX
#define osPiReadIo(X,Y) do { } while (0)
#define osPiGetStatus() 0
#endif

void sectors_to_ram(uint32_t ram, uint32_t start_sector, uint32_t count_sectors) {
    sprintf(
        message1,
        "copying to %08x from sector %u, number of sectors %u",
        ram,
        start_sector,
        count_sectors
    );
#ifdef LINUX
    printf("%s\n", message1);
#endif
}

/**
 * Load the file beginning at clus to the RAM beginning at ramaddr.
 */
void loadRomToRam(uint32_t ramaddr, uint32_t clus)
{
    uint32_t ram = ramaddr;

    uint32_t start_cluster = clus;
    uint32_t current_cluster = start_cluster;
    uint32_t next_cluster;

    uint32_t start_sector;
    uint32_t clusters;
    
    while (start_cluster < 0x0ffffff8) {
        next_cluster = fat_get_fat(current_cluster);

        // contiguous so far, keep copying
        if (next_cluster != current_cluster + 1)  
        {
            start_sector = CLUSTER_TO_SECTOR(start_cluster);
            clusters = current_cluster - start_cluster + 1;
            cfSectorsToRam(ram, start_sector, clusters * fat_fs.sect_per_clus);

            start_cluster = next_cluster;
            ram += clusters * fat_fs.sect_per_clus * 512 / 2;
        }

        current_cluster = next_cluster;
    }

    return;
}









/**
 * Locate menu.bin in the root dir and load it into ram.
 */
int fatLoadTable()
{
    fat_dirent de;
    int ret = -1;

    fat_root_dirent(&de);

    // read the directory
    do{
        ret = fat_readdir(&de);

        if (strcmp(de.short_name, "MENU.BIN") == 0)     break;
    }while (ret > 0);

    // either we reached EOD or there was an error
    if (ret <= 0) {
        if (ret == 0)
            sprintf(message1, "MENU.BIN not found!");
        return 1;
    }

    if (de.directory) {
        sprintf(message1, "MENU.BIN abnormal!");
        return 1;
    }

    if (de.size == 0) {
        sprintf(message1, "MENU.BIN is empty");
        return 1;
    }

    // menu.bin exists, normal file
    sprintf(message1, "menu xfering, size %u", de.size);
    loadRomToRam(0x0, de.start_cluster);

    return 0;
}









#ifdef LINUX // local test version of CF read/write

void cfSectorToRam(uint32_t ramaddr, uint32_t lba) {
}

void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors) {
}

// read a sector 
void cfReadSector(unsigned char *buffer, uint32_t lba) {
    int ret = fseek(cf_file, lba * 512, SEEK_SET);
    if (ret < 0)
        goto error;

    size_t count = fread(buffer, 512, 1, cf_file);
    if (count != 1)
        goto error;

    return;

error:
    // if there's an error, return FF's
    memset(buffer, 0xff, 512);
}

void cfWriteSector(unsigned char *buffer, uint32_t lba) {
    printf("write to %08x\n", lba * 512);
    int ret = fseek(cf_file, lba * 512, SEEK_SET);
    if (ret < 0)
        goto error;

    size_t count = fwrite(buffer, 512, 1, cf_file);
    if (count != 1)
        goto error;

    fflush(cf_file);

    return;

error:
    abort();
}

#else // FPGA versions of CF read/write

void cfWaitCI()
{
    long timeout = 0;
    char buf[8];

    // poll the status register until it's good
    do{
        osPiReadIo(0xB8000200, buf); while(osPiGetStatus() != 0);
        timeout++;
        if(timeout == 50000000){
            sprintf(message1, "Timed out.");
            fail = 1;
            return;
        }
    }while((buf[0] | buf[1] | buf[2] | buf[3]) != 0);
}


void cfSectorToRam(uint32_t ramaddr, uint32_t lba)
{
    cfWaitCI();

    // write LBA to the fpga register
    osPiWriteIo(0xB8000000, lba ); while(osPiGetStatus() != 0);
    osPiWriteIo(0xB8000004, ramaddr); while(osPiGetStatus() != 0);

    // write "read sector to ram" command
    osPiWriteIo(0xB8000208, 2); while(osPiGetStatus() != 0);

    cfWaitCI();
}




void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors)
{
    cfWaitCI();

    // write LBA to the fpga register
    osPiWriteIo(0xB8000000, lba ); while(osPiGetStatus() != 0);
    osPiWriteIo(0xB8000004, ramaddr); while(osPiGetStatus() != 0);
    osPiWriteIo(0xB8000008, sectors); while(osPiGetStatus() != 0);

    // write "read sectors to ram" command
    osPiWriteIo(0xB8000208, 3); while(osPiGetStatus() != 0);

    cfWaitCI();
}

int cfOptimizeCycleTime()
{
    int cycletime = 40;

    // first sector MUST already be loaded into buffer

    // the algorithm is as follow:
    // first the cycletime is high, for slow access.
    // repeatedly set the cycletime smaller (for faster)
    // stop once the data doesn't match what you got for the first read.

    while(1)
    {
        cfReadSector(fat_buffer, 0);

        if(memcmp(fat_buffer, buffer, 512) != 0)
        {
            // newly read buffer differs from original! must be corrupted, slow back down
            cycletime += 5;
            cfSetCycleTime(cycletime);
            return cycletime;
        }
        // otherwise trim it down a bit
        cycletime -= 5;

        if(cycletime <= 5)
        {
            // never let it go below 5
            cfSetCycleTime(5);
            return 5;
        }
    }
}

void cfSetCycleTime(int cycletime)
{
    cfWaitCI();

    // write cycletime to the fpga register
    osPiWriteIo(0xB8000000, cycletime); while(osPiGetStatus() != 0);
    // write "read sector" command
    osPiWriteIo(0xB8000208, 0xfd); while(osPiGetStatus() != 0);

    cfWaitCI();
}

void cfReadSector(unsigned char *buffer, uint32_t lba)
{
    cfWaitCI();

    // write LBA to the fpga register
    osPiWriteIo(0xB8000000, lba); while(osPiGetStatus() != 0);
    // write "read sector" command
    osPiWriteIo(0xB8000208, 1); while(osPiGetStatus() != 0);

    cfWaitCI();

    // DANGER WILL ROBINSON
    // We are DMAing... if we don't write back all cached data, WE'RE FUCKED
    osWritebackDCacheAll();

    // read the 512-byte onchip buffer (m9k on the fpga)
    osPiRawStartDma(OS_READ, 0xB8000000, (u32)buffer, 512); while(osPiGetStatus() != 0);

    osPiReadIo(0xB0000000, (u32)buf); while(osPiGetStatus() != 0);
}

#endif

#ifdef LINUX

void test_find_create(void) {
    int ret;
    fat_dirent dir, result;

    fat_root_dirent(&dir);
    ret = fat_find_create("abcdefghijklmnopqrstuvqxyz.bin", &dir, &result, 0);

    if (ret == 0)
        printf("result: size %u, start %u\n", result.size, result.start_cluster);
}

void test_set_size(void) {
    int ret;
    fat_dirent dir, result;

    fat_root_dirent(&dir);
    ret = fat_find_create("1", &dir, &result, 0);

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

    cf_file = fopen(argv[1], "r+");
    if (cf_file == NULL)
        err(1, "Couldn't open %s for reading", argv[1]);

    ret = fatInit();
    if (ret != 0)
        errx(1, "%s", message1);

    // test_find_create();

    /*
    puts("testing set_size");
    test_set_size();
    */

    /*
    fat_debug_readdir(fat_fs.root_cluster);
    return 0;
    */

    ret = fatLoadTable();
    if (ret != 0)
        warnx("%s", message1);

    fat_dirent de;
    fat_root_dirent(&de);

    while ((ret = fat_readdir(&de)) > 0) {
        if (strcmp(de.long_name, "menu.bin") == 0)
            fat_get_sectors(de.start_cluster, sectors, 10);

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

    return 0;
}

#endif
