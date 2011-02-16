#include <stdio.h>
#include <string.h>

#include "common.h"

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
