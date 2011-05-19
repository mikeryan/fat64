#ifndef __CI_H__
#define __CI_H__

#define CI_BUFFER       0x18000000  // read / write
#define CI_STATUS       0x18000200  // read
#define CI_COMMAND      0x18000208  // write
#define CI_LBA          0x18000210  // write
#define CI_LENGTH       0x18000218  // write
#define CI_REVISION     0x180002fc  // read
#define CI_EEPROM       0x18001000  // read / write
#define CI_SAVE_WB      0x18001800  // read / write

#define CI_CMD_READ_SECTOR      0x01
#define CI_CMD_SECTOR_TO_SDRAM  0x02
#define CI_CMD_SECTORS_TO_SDRAM 0x03
#define CI_CMD_WRITE_SECTOR     0x10
#define CI_CMD_SET_SAVE_TYPE    0xd0
#define CI_CMD_DISABLE_BYTESWAP 0xe0
#define CI_CMD_ENABLE_BYTESWAP  0xe1
#define CI_CMD_ENABLE_SDRAM_WR  0xf0
#define CI_CMD_DISABLE_SDRAM_WR 0xf1
#define CI_CMD_SET_CYCLETIME    0xfd

#endif /* __CI_H__ */
