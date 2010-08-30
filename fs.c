#ifdef LINUX

#include <err.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int uint32_t;

// the disk image
FILE *cf_file;

typedef struct _fat_dirent {
    // file properties
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

uint32_t current_lba = -1;

unsigned char buffer[512];
char message1[BUFSIZ];

unsigned char fat_buffer[512];
unsigned long fat_buffer_sector = -1;

uint32_t fs_begin_sector;

typedef struct _fat_fs_t {
    uint32_t begin_sector;
    uint32_t sect_per_clus;
    uint32_t root_cluster;
    uint32_t clus_begin_sector;
} fat_fs_t;

fat_fs_t fat_fs;

int             compat_mode = 0;

void loadRomToRam(uint32_t ramaddr, uint32_t clus);

void cfSectorToRam(uint32_t ramaddr, uint32_t lba);
void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors);
void cfReadSector(unsigned char *buffer, uint32_t lba);

void cfSetCycleTime(int cycletime);
int cfOptimizeCycleTime();

void fat_sector_offset(uint32_t cluster, uint32_t *fat_sector, uint32_t *fat_offset);
uint32_t fat_get_fat(uint32_t cluster);

uint32_t intEndian(unsigned char *i);
unsigned short shortEndian(unsigned char *i);
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

int fatInit()
{
    // can be localized to fatInit
    char fat_systemid[8];
    uint32_t fat_num_resv_sect;
    uint32_t fat_num_fats;
    uint32_t fat_sect_per_fat;

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
    fat_num_fats = buffer[0x10];
    fat_sect_per_fat = intEndian(&buffer[0x24]);
    fat_fs.root_cluster = intEndian(&buffer[0x2c]);

    fat_fs.begin_sector = fs_begin_sector + fat_num_resv_sect;
    fat_fs.clus_begin_sector = fat_fs.begin_sector + (fat_num_fats * fat_sect_per_fat);

    sprintf(message1, "Loaded successfully.");

    return 0;
}

/**
 * Get the sector # and offset into the sector for a given cluster.
 */
void fat_sector_offset(uint32_t cluster, uint32_t *fat_sector, uint32_t *fat_offset) {
    uint32_t index = cluster * 4;   // each cluster is 4 bytes long
    uint32_t sector = index / 512;  // 512 bytes per sector, rounds down

    *fat_sector = fat_fs.begin_sector + sector;
    *fat_offset = index - sector * 512;
}

/**
 * Get the FAT entry for a given cluster.
 */
uint32_t fat_get_fat(uint32_t cluster) {
    uint32_t sector;
    uint32_t offset;

    // get the sector of the FAT and offset into the sector
    fat_sector_offset(cluster, &sector, &offset);

    // only read sector if it has changed! saves time
    if(sector != fat_buffer_sector){
        // read the sector
        cfReadSector(fat_buffer, sector);
        fat_buffer_sector = sector;
    }

    return intEndian(&fat_buffer[offset]);
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

// first sector of a cluster
#define CLUSTER_TO_SECTOR(X) ( fat_fs.clus_begin_sector + (X - 2) * fat_fs.sect_per_clus )

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

    uint32_t sector;
    uint32_t offset;
    uint32_t attributes;
    uint32_t segment;
    uint32_t fat_entry;

    char *dest;

    if (dirent->index > 512 / 32) {
        sprintf(message1, "Invalid directory");
        return -1;
    }

    if (dirent->long_name)
        memset(dirent->long_name, 0, 256);

    do {
        if (dirent->index == 512/32) {
            dirent->index = 0;
            ++dirent->sector;

            // load the next cluster once we reach the end of this
            if (dirent->sector == fat_fs.sect_per_clus) {
                // look up the cluster number in the FAT
                fat_entry = fat_get_fat(dirent->cluster);
                if (fat_entry >= 0x0ffffff8) // last cluster
                    goto end_of_dir;

                dirent->cluster = fat_entry;
                dirent->sector = 0;
            }
        }

        sector = CLUSTER_TO_SECTOR(dirent->cluster) + dirent->sector;
        cfReadSector(buffer, sector);

        offset = dirent->index * 32;

        // end of directory reached
        if (buffer[offset] == 0)
            goto end_of_dir;

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

    return 1;

end_of_dir: // end of directory reached
    // reset the metadata to point to the beginning of the dir
    dirent->index = 0;
    dirent->cluster = dirent->first_cluster;
    dirent->sector = 0;

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
            "%-12s (%c) %5d %s\n",
            de.short_name, de.directory ? 'd' : 'f',
            de.start_cluster,
            de.long_name
        );

    if (ret < 0)
        errx(1, "%s", message1);

    return 0;
}

#endif
