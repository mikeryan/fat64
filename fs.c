#ifdef DEBUG

#include <stdio.h>
#include <string.h>

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
    int t = ((int)*(i+1)<<8) |
            ((int)*(i+0));
    return t;
}

// 4-byte number
unsigned int intEndian(unsigned char *i)
{
    unsigned int t = ((int)*(i+3)<<24) |
            ((int)*(i+2)<<16) |
            ((int)*(i+1)<<8) |
            ((int)*(i+0));

    return t;
}

int fatInit()
{
    unsigned char temp[32];

    // read first sector
    cfReadSector(buffer, 0);

    // check for MBR/VBR magic
    if( !(buffer[0x1fe]==0x55 && buffer[0x1ff]==0xAA) ){
        sprintf(message1, "no cf card or bad filesystem");
        return 1;
    }

    // look for 'FAT'
    if(strncmp(&buffer[82], "FAT", 3) == 0)
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









int fatLoadTable()
{
    unsigned char dir_data[32];
    short dir_num = 0;

    unsigned int current_clus_num = fat_root_dir_first_clus;
    unsigned int current_lba;
    unsigned int sector_count = 0;
    unsigned int next_clus_num;

    dir_num_files = 0;

    //temp = 546787;

    // grab root dir
    current_lba = fat_clus_begin_lba;
    cfReadSector(buffer, current_lba);
    while(1){
        // read the directory into the dir buffer
        memcpy(dir_data, buffer+dir_num*32, 32);

        // look at the attribute bit and find what kind of entry it is

        if((dir_data[11] & 15) == 15){
            // long filename text (all four type bits set)
            sprintf(message1, "long filename");
        }else if(dir_data[0] == 0xe5){
            // unused

        }else if(dir_data[0] == 0){
            // end of directory
            sprintf(message1, "menu.bin not found!");
            return 1;
        }else if((dir_data[11] & 8) == 8){
            // volume id
            memcpy(fat_volumeid, dir_data, 11);
            fat_volumeid[11] = 0;                                       // null-terminated
        }else{
            // normal directory entry! whew

            if(dir_data[11] & 1){
                // read only
            }
            if(dir_data[11] & 2){
                // hidden
            }
            if(dir_data[11] & 4){
                // system
            }

            if(dir_data[11] & 16){
                // directory
            }
            if(dir_data[11] & 32){
                // archive
            }

            // we are looking for a file that's not a folder or volid
            //if( !((dir_data[11] & 24)==24) ){
                rom_size = intEndian(&dir_data[0x1c]);
                if(strncmp(dir_data, "MENU    BIN", 11) == 0)
                {
                    sprintf(message1, "menu xfering, size %lu", rom_size);
                    current_clus_num = (int)dir_data[0x15] << 24 |
                                  (int)dir_data[0x14] << 16 |
                                  (int)dir_data[0x1b] << 8 |
                                  (int)dir_data[0x1a] ;

                    loadRomToRam(0x0, current_clus_num);

                    sprintf(message1, "current_clus %u", current_clus_num);
                    return 0;
                }
                dir_num_files ++;
            //}

        }

        dir_num++;
        if(dir_num == 16){
            // reached the end of the sector, get the next one
            dir_num = 0;

            current_lba++;
            cfReadSector(buffer, current_lba);

            // continue until we reach end of the cluster
            sector_count++;
            if(sector_count == fat_sect_per_clus){
                // read the sector of FAT that contains the next cluster for this file
                cfReadSector(buffer, fat_begin_lba + (current_clus_num >> 7));

                next_clus_num = intEndian(&buffer[(current_clus_num-(current_clus_num / 128)*128) * 4]) & 0x0FFFFFFF;

                if (next_clus_num >= 0x0ffffff8 ) {
                    sprintf(message1, "menu.bin not found!");
                    return 1;
                }else{
                    current_clus_num = next_clus_num;
                }
                cfReadSector(buffer, fat_clus_begin_lba + ((current_clus_num - fat_root_dir_first_clus) * fat_sect_per_clus));
            }
        }
    }
}









#ifdef DEBUG // local test version of CF read/write

void cfSectorToRam(int ramaddr, int lba) {
}

void cfReadSector(unsigned char *buffer, int lba) {
}

#else // FPGA versions of CF read/write

void cfSectorToRam(int ramaddr, int lba)
{
    char buf[8];
    long timeout = 0;

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

int main(void) {
    return 0;
}

#endif
