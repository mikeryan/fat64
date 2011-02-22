#ifndef __FS_H__
#define __FS_H__

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef struct _fat_file_t fat_file_t;

char *fat_errstr(int code);

#define FAT_SUCCESS 0
#define FAT_NOSPACE 1
#define FAT_EOF 2
#define FAT_NOTFOUND 3
#define FAT_INCONSISTENT 256

int fat_init(void);

int fat_root(fat_file_t *file);

int fat_open(char *filename, fat_file_t *folder, char *flags, fat_file_t *file);
uint32_t fat_read(fat_file_t *file, unsigned char *buf, uint32_t len);

#endif /* __FS_H__ */
