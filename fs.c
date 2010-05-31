#ifdef DEBUG

#include <err.h>
#include <stdio.h>
#include <string.h>

// the disk image
FILE *cf_file;

typedef struct _fat_dirent {
    // file properties
    char short_name[13];
    int directory;
    unsigned start_cluster;
    unsigned size;

    // metadata
    int index;
    int cluster;
    int sector;

    int first_cluster;
} fat_dirent;

int current_lba = -1;

unsigned char buffer[512];
char message1[BUFSIZ];

unsigned long fat_begin_lba;
unsigned long fat_sect_per_clus;
unsigned long fat_root_dir_first_clus;
unsigned long fat_clus_begin_lba;
unsigned long rom_size;

// can be localized to fatInit
unsigned long fat_part_begin_lba;
char fat_systemid[8];
unsigned long fat_num_resv_sect;
unsigned long fat_num_fats;
unsigned long fat_sect_per_fat;

// can be localized to fatLoadTable
unsigned long dir_num_files;
char fat_volumeid[12];

void cfSectorToRam(int ramaddr, int lba);
void cfReadSector(unsigned char *buffer, int lba);

#endif

// 2-byte number
unsigned short shortEndian(unsigned char *i)
{
// assume little endian system for debug
#ifdef DEBUG
    return *(unsigned short *)i;
#else

    int t = ((int)*(i+1)<<8) |
            ((int)*(i+0));
    return t;
#endif
}

// 4-byte number
unsigned int intEndian(unsigned char *i)
{
#ifdef DEBUG
    return *(unsigned *)i;
#else
    unsigned int t = ((int)*(i+3)<<24) |
            ((int)*(i+2)<<16) |
            ((int)*(i+1)<<8) |
            ((int)*(i+0));

    return t;
#endif
}

int fatInit()
{
    // read first sector
    cfReadSector(buffer, 0);

    // check for MBR/VBR magic
    if( !(buffer[0x1fe]==0x55 && buffer[0x1ff]==0xAA) ){
        sprintf(message1, "no cf card or bad filesystem");
        return 1;
    }

    // look for 'FAT'
    if(strncmp((char *)&buffer[82], "FAT", 3) == 0)
    {
        // this first sector is a Volume Boot Record
        fat_part_begin_lba = 0;
    }else{
        // this is a MBR. Read first entry from partition table
        fat_part_begin_lba = intEndian(&buffer[0x1c6]);
    }

    cfReadSector(buffer, fat_part_begin_lba);

    // copy the system ID string
    memcpy(fat_systemid, &buffer[82], 8);
    fat_systemid[8] = 0;

    // check for antique FATs
    if(strncmp(fat_systemid, "FAT12", 5) == 0){
        sprintf(message1, "FAT12 format, won't work.");
        return 1;
    }
    if(strncmp(fat_systemid, "FAT16", 5) == 0){
        sprintf(message1, "FAT16 format, won't work.");
        return 1;
    }
    if(strncmp(fat_systemid, "FAT32", 5) != 0){
        // not a fat32 volume
        sprintf(message1, "FAT32 partition not found.");
        return 1;
    }

    fat_sect_per_clus = buffer[0x0d];
    fat_num_resv_sect = shortEndian(&buffer[0x0e]);
    fat_num_fats = buffer[0x10];
    fat_sect_per_fat = intEndian(&buffer[0x24]);
    fat_root_dir_first_clus = intEndian(&buffer[0x2c]);

    fat_begin_lba = fat_part_begin_lba + fat_num_resv_sect;
    fat_clus_begin_lba = fat_begin_lba + (fat_num_fats * fat_sect_per_fat);

    sprintf(message1, "loaded successfully.");

    return 0;
    //sprintf(message1, "%s, %u, %u, %u, %u, %u, %u", fat_systemid, fat_part_begin_lba, fat_sect_per_clus, fat_num_resv_sect, fat_num_fats, fat_sect_per_fat, fat_root_dir_first_clus);
}

/**
 * Get the sector # and offset into the sector for a given cluster.
 */
void fat_sector_offset(int cluster, unsigned *fat_sector, unsigned *fat_offset) {
    unsigned index = cluster * 4;   // each cluster is 4 bytes long
    unsigned sector = index / 512;  // 512 bytes per sector, rounds down

    *fat_sector = fat_begin_lba + sector;
    *fat_offset = index - sector * 512;
}

/**
 * Get the FAT entry for a given cluster.
 */
unsigned fat_get_fat(int cluster) {
    unsigned sector, offset;

    // get the sector of the FAT and offset into the sector
    fat_sector_offset(cluster, &sector, &offset);

    // read the sector
    cfReadSector(buffer, sector);

    return intEndian(buffer + offset);
}

/**
 * Get the root directory entry.
 */
int fat_root_dirent(fat_dirent *dirent) {
    dirent->short_name[0] = 0;
    dirent->directory = 0;
    dirent->start_cluster = 0;
    dirent->size = 0;

    dirent->index = 0;
    dirent->cluster = fat_root_dir_first_clus;
    dirent->sector = 0;
    dirent->first_cluster = fat_root_dir_first_clus;

    return 0;
}

// first sector of a cluster
#define CLUSTER_TO_SECTOR(X) ( fat_clus_begin_lba + (X - 2) * fat_sect_per_clus )

/**
 * Read a directory.
 * returns:
 *   1  success
 *   0  end of dir
 *  -1  error
 */
int fat_readdir(fat_dirent *dirent) {
    int found_file = 0;
    int i, j;

    if (dirent->index > 512 / 32) {
        sprintf(message1, "invalid dirent");
        return -1;
    }

    do {
        if (dirent->index == 512/32) {
            dirent->index = 0;
            ++dirent->sector;

            // load the next cluster once we reach the end of this
            if (dirent->sector == fat_sect_per_clus) {
                // look up the cluster number in the FAT
                unsigned fat_entry = fat_get_fat(dirent->cluster);
                if (fat_entry >= 0x0ffffff8) // last cluster
                    goto end_of_dir;

                dirent->cluster = fat_entry;
                dirent->sector = 0;
            }
        }

        int sector = CLUSTER_TO_SECTOR(dirent->cluster) + dirent->sector;
        cfReadSector(buffer, sector);

        int offset = dirent->index * 32;

        // end of directory reached
        if (buffer[offset] == 0)
            goto end_of_dir;

        ++dirent->index;

        // deleted file, skip
        if (buffer[offset] == 0xe5)
            continue;

        int attributes = buffer[offset + 0x0b];

        // long filename, skip
        if (attributes == 0x0f)
            continue;

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

    return 1;

end_of_dir: // end of directory reached
    // reset the metadata to point to the beginning of the dir
    dirent->index = 0;
    dirent->cluster = dirent->first_cluster;
    dirent->sector = 0;

    return 0;
}

#ifdef DEBUG

void loadRomToRam(int ramaddr, int clus) {
}

#else

void loadRomToRam(int ramaddr, int clus)
{
    unsigned int ram = ramaddr;

    unsigned int current_clus_num = clus;
    unsigned int next_clus_num;
    unsigned int current_lba = fat_clus_begin_lba + ((current_clus_num - fat_root_dir_first_clus) * fat_sect_per_clus);

    unsigned long sector_count = 0;

    while(1)
    {
        // transfer 512bytes to SDRAM
        cfSectorToRam(ram, current_lba);
        ram += 256;
        current_lba++;
        sector_count++;

        if(ram*2 >= rom_size) return;
        if(sector_count == fat_sect_per_clus)
        {
            // read the sector of FAT that contains the next cluster for this file
            cfReadSector(buffer, fat_begin_lba + (current_clus_num / 128));
            next_clus_num = intEndian(&buffer[(current_clus_num-(current_clus_num / 128)*128) * 4]) & 0x0FFFFFFF;

            if (next_clus_num >= 0x0ffffff8 ) {
                return;
            }else{
                current_clus_num = next_clus_num;
            }
            sector_count = 0;
            current_lba = fat_clus_begin_lba + ((current_clus_num - fat_root_dir_first_clus) * fat_sect_per_clus);
        }
    }
    osPiReadIo(0xB0000000, buffer); while(osPiGetStatus() != 0);


}

#endif









int fatLoadTable()
{
    fat_dirent de;
    int ret;

    fat_root_dirent(&de);

    // read the directory
    while ((ret = fat_readdir(&de)) > 0)
        // break if we find menu.bin
        if (strcmp(de.short_name, "MENU.BIN") == 0)
            break;

    // either we reached EOD or there was an error
    if (ret <= 0) {
        if (ret == 0)
            sprintf(message1, "menu.bin not found!");
        return 1;
    }

    if (de.directory) {
        sprintf(message1, "menu.bin is not a normal file");
        return 1;
    }

    if (de.size == 0) {
        sprintf(message1, "menu.bin is empty");
        return 1;
    }

    // menu.bin exists, normal file
    sprintf(message1, "menu xfering, size %u", de.size);
    loadRomToRam(0x0, de.start_cluster);

    return 0;
}









#ifdef DEBUG // local test version of CF read/write

void cfSectorToRam(int ramaddr, int lba) {
}

// read a sector 
void cfReadSector(unsigned char *buffer, int lba) {
    if (current_lba == lba)
        return;

    int ret = fseek(cf_file, lba * 512, SEEK_SET);
    if (ret < 0)
        goto error;

    size_t count = fread(buffer, 512, 1, cf_file);
    if (count != 1)
        goto error;

    current_lba = lba;
    return;

error:
    // if there's an error, return FF's
    memset(buffer, 0xff, 512);
}

#else // FPGA versions of CF read/write

void cfSectorToRam(int ramaddr, int lba)
{
    char buf[8];
    long timeout = 0;

    if (current_lba == lba)
        return;

    // first poll the status register until it's good
    do{
        osPiReadIo(0xB8000200, buf); while(osPiGetStatus() != 0);
        timeout++;
        if(timeout == 60000000){
            sprintf(message1, "timed out. (sectoram1)");
            return;
        }
    }while((buf[0] | buf[1] | buf[2] | buf[3]) != 0);

    // write LBA to the fpga register
    osPiWriteIo(0xB8000000, lba ); while(osPiGetStatus() != 0);
    osPiWriteIo(0xB8000004, ramaddr); while(osPiGetStatus() != 0);

    // write "read sector to ram" command
    osPiWriteIo(0xB8000208, 2); while(osPiGetStatus() != 0);

    current_lba = lba;
}








void cfReadSector(unsigned char *buffer, int lba)
{
    char buf[8];
    long timeout = 0;

    // first poll the status register until it's good
    do{
        osPiReadIo(0xB8000200, buf); while(osPiGetStatus() != 0);
        timeout++;
        if(timeout == 60000000){
            sprintf(message1, "timed out. (readsector1)");
            return;
        }
    }while((buf[0] | buf[1] | buf[2] | buf[3]) != 0);

    // write LBA to the fpga register
    osPiWriteIo(0xB8000000, lba); while(osPiGetStatus() != 0);
    // write "read sector" command
    osPiWriteIo(0xB8000208, 1); while(osPiGetStatus() != 0);

    // poll the status register until it's good
    do{
        osPiReadIo(0xB8000200, buf); while(osPiGetStatus() != 0);
        timeout++;
        if(timeout == 60000000){
            sprintf(message1, "timed out. (readsector2)");
            return;
        }
    }while((buf[0] | buf[1] | buf[2] | buf[3]) != 0);

    // DANGER WILL ROBINSON
    // We are DMAing... if we don't write back all cached data, WE'RE FUCKED
    osWritebackDCacheAll();

    // read the 512-byte onchip buffer (m9k on the fpga)
    osPiRawStartDma(OS_READ, 0xB8000000, (u32)buffer, 512); while(osPiGetStatus() != 0);

    osPiReadIo(0xB0000000, (u32)buf); while(osPiGetStatus() != 0);
}

#endif

#ifdef DEBUG

int main(int argc, char **argv) {
    int ret;

    if (argc < 2) {
        printf("Usage: %s <file_system.img>\n", argv[0]);
        return 1;
    }

    cf_file = fopen(argv[1], "r");
    if (cf_file == NULL)
        err(1, "Couldn't open %s for reading", argv[1]);

    ret = fatInit();
    if (ret != 0)
        errx(1, "%s", message1);

    ret = fatLoadTable();
    if (ret != 0)
        errx(1, "%s", message1);

    fat_dirent de;
    fat_root_dirent(&de);

    while ((ret = fat_readdir(&de)) > 0)
        printf(
            "%-12s (%c) %5d\n",
            de.short_name, de.directory ? 'd' : 'f',
            de.start_cluster
        );

    if (ret < 0)
        errx(1, "%s", message1);

    return 0;
}

#endif
