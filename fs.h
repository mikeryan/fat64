#ifndef __FS_H__
#define __FS_H__

#include <stdint.h>

typedef struct _fat_file_t fat_file_t;

char *fat_errstr(int code);

#define FAT_SUCCESS 0
#define FAT_NOSPACE 1
#define FAT_EOF 2
#define FAT_NOTFOUND 3
#define FAT_INCONSISTENT 256

int fat_init(void);

int fat_root(fat_file_t *file);

// file operations
int fat_open(const char *filename, fat_file_t *folder, char *flags, fat_file_t *file);
int32_t fat_read(fat_file_t *file, unsigned char *buf, int32_t len);
int fat_seek(fat_file_t *file, uint32_t position);
int fat_file_isdir(fat_file_t *file);
uint32_t fat_file_size(fat_file_t *file);

#endif /* __FS_H__ */
