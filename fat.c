#include "common.h"

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
void fat_flush_fat(void) {
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
    cfReadSector(fs_info, fat_fs.info_sector);
    old_free = intEndian(&fs_info[0x1e8]);
    if (old_free != fat_fs.free_clusters) {
        writeInt(&fs_info[0x1e8], fat_fs.free_clusters);
        cfWriteSector(fs_info, fat_fs.info_sector);
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
        fat_flush_fat();

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
int fat_allocate_cluster(uint32_t last_cluster, uint32_t *new_cluster) {
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


