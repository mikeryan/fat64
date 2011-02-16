#include "common.h"

#ifdef LINUX // local test version of CF read/write

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the disk image
FILE *cf_file;

#define osPiReadIo(X,Y) do { } while (0)
#define osPiGetStatus() 0

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

/**
 * Open a FAT disk image.
 */
void fat_disk_open(char *filename) {
    cf_file = fopen(filename, "r+");
    if (cf_file == NULL)
        err(1, "Couldn't open %s for reading", filename);
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
