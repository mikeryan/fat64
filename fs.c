#ifdef LINUX

#include <stdio.h>
#include <string.h>

#include "common.h"

unsigned char buffer[512];
char message1[4096];

unsigned char fat_buffer[512];
uint32_t fat_buffer_sector = -1;
int fat_buffer_dirty = 0;

uint32_t dir_buffer_sector = 0;
int dir_buffer_dirty = 0;

uint32_t fs_begin_sector;

fat_fs_t fat_fs;

int             compat_mode = 0;

void fat_sector_offset(uint32_t cluster, uint32_t *fat_sector, uint32_t *fat_offset);

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

/**
 * Returns a human-readable string for one of the above error codes.
 */
char *fat_errstr(int code) {
    switch (code) {
        case FAT_SUCCESS:
            return "success";
        case FAT_NOSPACE:
            return "file system full";
        case FAT_EOF:
            return "end of file";
        case FAT_NOTFOUND:
            return "file not found";
        case FAT_INCONSISTENT:
            return "inconsistent file system";
        default:
            break;
    }
    return "unknown error";
}

/**
 * Init the file system.
 *
 * Returns:
 *  0   success
 *  1   failure, with message in message1 (FIXME)
 */
int fat_init(void) {
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

    fat_fs.info_sector = fs_begin_sector + 1;
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
 * Return the sector and offset into the sector of a file given start cluster
 * and offset.
 *
 * Returns:
 *  FAT_SUCCESS on success
 *  FAT_EOF when the end of file is reached
 */
int fat_get_sector(uint32_t start_cluster, uint32_t offset, uint32_t *sector, uint32_t *new_offset) {
    uint32_t bytes_per_clus = fat_fs.sect_per_clus * 512;
    uint32_t cluster = start_cluster;

    // skip to the right cluster
    while (offset >= bytes_per_clus) {
        cluster = fat_get_fat(cluster);

        // hit the end of the file
        if (cluster >= 0x0ffffff7)
            return FAT_EOF;

        offset -= bytes_per_clus;
    }

    *sector = CLUSTER_TO_SECTOR(cluster);
    while (offset >= 512) {
        ++*sector;
        offset -= 512;
    }
    *new_offset = offset;

    return FAT_SUCCESS;
}
