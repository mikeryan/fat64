#ifndef __COMMON_H__
#define __COMMON_H__

#include "fs.h"

/************************
 * CONSTANTS AND MACROS *
 ************************/

// first sector of a cluster
#define CLUSTER_TO_SECTOR(X) ( fat_fs.clus_begin_sector + (X - 2) * fat_fs.sect_per_clus )

// dirents per sector
#define DE_PER_SECTOR (512 / 32)

/**************
 * STRUCTURES *
 **************/
typedef struct _fat_fs_t {
    uint32_t info_sector;
    uint32_t begin_sector;
    uint32_t sect_per_clus;
    uint32_t num_fats;
    uint32_t sect_per_fat;
    uint32_t root_cluster;
    uint32_t clus_begin_sector;
    uint32_t total_clusters;
    uint32_t free_clusters;
} fat_fs_t;

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

/*
 * Eventually we can whittle fat_file_t down to this, but for now the typedef
 * makes implementation simpler.
typedef struct _fat_file_t {
    char *name;
    char short_name[13];
    char long_name[256];
    int directory;
    uint32_t start_cluster;
    uint32_t size;
} fat_file_t;
*/
struct _fat_file_t {
    fat_dirent de;
    uint32_t position;
};

/*************
 * FUNCTIONS *
 *************/

/*
 * Bits and bytes
 */
uint32_t intEndian(unsigned char *i);
void writeInt(unsigned char *dest, uint32_t val);
unsigned short shortEndian(unsigned char *i);
void writeShort(unsigned char *dest, uint16_t val);

/**
 * Files
 */
int fat_find_create(char *filename, fat_dirent *folder, fat_dirent *result_de, int dir, int create);
int fat_set_size(fat_dirent *de, uint32_t size);

/*
 * FAT
 */
uint32_t fat_get_fat(uint32_t cluster);
void fat_set_fat(uint32_t cluster, uint32_t value);
int fat_allocate_cluster(uint32_t last_cluster, uint32_t *new_cluster);
void fat_flush_fat(void);

/*
 * Directories
 */
int fat_root_dirent(fat_dirent *dirent);
int fat_readdir(fat_dirent *dirent);
void fat_debug_readdir(uint32_t start_cluster);
int fat_allocate_dirents(fat_dirent *dirent, int count);
void fat_init_dir(uint32_t cluster, uint32_t parent);
void fat_sub_dirent(uint32_t start_cluster, fat_dirent *de);
int fat_dir_create_file(char *filename, fat_dirent *folder, fat_dirent *result_de, int dir);

// FIXME these should not be global
// move them back into dir.c and make them static again
void _fat_flush_dir(void);
void _fat_write_dirent(fat_dirent *de);

/*
 * Disk
 */
void cfSectorToRam(uint32_t ramaddr, uint32_t lba);
void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors);
void cfReadSector(unsigned char *buffer, uint32_t lba);
void cfWriteSector(unsigned char *buffer, uint32_t lba);

void cfSetCycleTime(int cycletime);
int cfOptimizeCycleTime();

void fat_disk_open(char *filename);

/*
 * Etc.
 */
int fatInit();

int fat_get_sectors(uint32_t start_cluster, uint32_t *sectors, int size);
int fat_get_sector(uint32_t start_cluster, uint32_t offset, uint32_t *sector, uint32_t *new_offset);

// from fs.c
extern char message1[4096];
extern fat_fs_t fat_fs;

// directory buffer
extern unsigned char buffer[512];
extern uint32_t dir_buffer_sector;
extern int dir_buffer_dirty;

// fat buffer
extern unsigned char fat_buffer[512];
extern uint32_t fat_buffer_sector;
extern int fat_buffer_dirty;

#endif /* __COMMON_H__ */
