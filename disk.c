#include <string.h>

#include "common.h"
#include "ci.h"

#ifdef LINUX

/****************************
 * stdio-based CF functions *
 ***************************/

#include <err.h>
#include <stdio.h>
#include <stdlib.h>

// the disk image
FILE *cf_file;

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

#else

/*****************
 * 64drive CF IO *
 ****************/

#include <libdragon.h>


/* Wait for CI status to be 0.
 * Run before and after each command. */
void ci_status_wait(void) {
    while ((io_read(CI_STATUS) >> 24) != 0)
        ;
}

void cfSectorToRam(uint32_t ramaddr, uint32_t lba)
{
    ci_status_wait();

    // write LBA to the fpga register
    io_write(CI_LBA, lba);

    // write the SDRAM address to the RAM register
    io_write(CI_SDRAM_ADDR, ramaddr);

    // write "read sector to ram" command
    io_write(CI_COMMAND, CI_CMD_READ_SECTOR);

    ci_status_wait();
}

void cfSectorsToRam(uint32_t ramaddr, uint32_t lba, int sectors)
{
    ci_status_wait();

    // write LBA, destination RAM, and length to the fpga registers
    io_write(CI_LBA, lba);
    io_write(CI_SDRAM_ADDR, ramaddr);
    io_write(CI_LENGTH, sectors);

    // write "read sectors to ram" command
    io_write(CI_COMMAND, CI_CMD_SECTORS_TO_SDRAM);

    ci_status_wait();
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
    ci_status_wait();

    // write cycletime to the fpga register
    io_write(CI_BUFFER, cycletime / 2); // divided by 2: kludge for 100 MHz -> 50 MHz
    // write "read sector" command
    io_write(CI_COMMAND, CI_CMD_SET_CYCLETIME);

    ci_status_wait();
}

void ciSetByteSwap(int byteswap)
{
    int cmd = byteswap ? CI_CMD_ENABLE_BYTESWAP : CI_CMD_DISABLE_BYTESWAP;

    ci_status_wait();
    io_write(CI_COMMAND, cmd);
    ci_status_wait();
}

void ciSetSave(int savetype)
{
    ci_status_wait();

    io_write(CI_BUFFER, savetype);
    io_write(CI_COMMAND, CI_CMD_SET_SAVE_TYPE);

    ci_status_wait();
}

void ciSetPersistentVar(unsigned int var)
{
    ci_status_wait();
    io_write(CI_PERSISTENT, var);
    ci_status_wait();
}
 
unsigned int ciGetPersistentVar()
{
    unsigned int ret = 0;

    ci_status_wait();
    ret = io_read(CI_PERSISTENT);
    ci_status_wait();
   
    return ret;
}

/* Based on libdragon code for cache invalidation */
#define cache_op(op) \
    addr=(void*)(((unsigned long)addr)&(~3));\
    for (;length>0;length-=4,addr+=4) \
	asm ("\tcache %0,(%1)\n"::"i" (op), "r" (addr))

static void _invalidate(volatile void * addr, unsigned long length)
{
    cache_op(0x1); // a little bird told me
}
 
void cfReadSector(unsigned char *buffer, uint32_t lba)
{
    ci_status_wait();

    // write LBA to the fpga register
    io_write(CI_LBA, lba);

    // write "read sector" command
    io_write(CI_COMMAND, CI_CMD_READ_SECTOR);

    ci_status_wait();

    // DANGER WILL ROBINSON
    // We are DMAing... if we don't write back all cached data, WE'RE FUCKED
    _invalidate(buffer, 512);

    // read the 512-byte onchip buffer (m9k on the fpga)
    dma_read((void *)((uint32_t)buffer & 0x1fffffff), CI_BUFFER, 512);
}

void cfWriteSector(unsigned char *buffer, uint32_t lba) {
    ci_status_wait();

    io_write(CI_LBA, lba);

    data_cache_writeback_invalidate(buffer, 512);
    dma_write((void *)((uint32_t)buffer & 0x1fffffff), CI_BUFFER, 512);
    data_cache_writeback_invalidate(buffer, 512);

    io_write(CI_COMMAND, CI_CMD_WRITE_SECTOR);

    ci_status_wait();
}

// copy N64 RDRAM into 64drive EEPROM emulator
void ciWriteEEPROMBuffer(unsigned char *buf, int start, int size)
{
    ci_status_wait();

    data_cache_writeback_invalidate(buffer, 512);
    dma_write((void *)((uint32_t)buffer & 0x1fffffff), CI_EEPROM + start, size);
    data_cache_writeback_invalidate(buffer, 512);

    ci_status_wait();
}

// copy N64 RDRAM into 64drive SRAM emulator
void ciWriteLBAWBBuffer(unsigned char *buf, int start, int size)
{
    ci_status_wait();

    // write the buffer
    data_cache_writeback_invalidate(buffer, 512);
    dma_write((void *)((uint32_t)buffer & 0x1fffffff), CI_SAVE_WB + start, size);
    data_cache_writeback_invalidate(buffer, 512);
}

#endif
