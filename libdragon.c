#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef LINUX
// #include "libdragon.h"
#include "system.h"
// #include "dfsinternal.h"
#endif

#include "fs.h"
#include "common.h"

#define MAX_OPEN_FILES          16
#define MAX_DIRECTORY_DEPTH     16
#define MAX_FILENAME_LEN        255

#define FLAGS_FILE          0x0
#define FLAGS_DIR           0x1
#define FLAGS_EOF           0x2

typedef struct _open_file_t {
    fat_file_t file;
    int open;
} open_file_t;

/* Internal filesystem stuff */
static open_file_t open_files[MAX_OPEN_FILES] = { { .open = 0 } };
static fat_dirent next_entry;
int valid_dir = 0;

/* File lookup*/
static fat_file_t *find_free_file(int *num)
{
    int i;

    for(i = 0; i < MAX_OPEN_FILES; i++)
    {
        if(!open_files[i].open)
        {
            /* Found one! */
            *num = i;
            open_files[i].open = 1;
            return &open_files[i].file;
        }
    }

    /* No free files */
    return NULL;
}

static fat_file_t *find_open_file(uint32_t x)
{
    if (x < MAX_OPEN_FILES && open_files[x].open)
        return &open_files[x].file;

    /* no such file */
    return 0;
}

/* Find the first file or directory in a directory listing.  Supports absolute
   and relative.  If the path is invalid, returns a negative DFS_errno.  If
   a file or directory is found, returns the flags of the entry and copies the
   name into buf. */
static int fat64_dir_findfirst(const char * const path, char *buf)
{
    int ret = fat_recurse_path(path, &next_entry, NULL, TYPE_DIR);

    /* Ensure that if this fails, they can't call findnext */
    valid_dir = 0;
    // next_entry = 0;

    if(ret != FAT_SUCCESS)
    {
        /* File not found, or other error */
        return -ret;
    }

    ret = fat_readdir(&next_entry);
    if (ret != 1)
        return -1;

    if(buf)
    {
        strcpy(buf, next_entry.name);
    }

    valid_dir = 1;

    return next_entry.directory ? FLAGS_DIR : FLAGS_FILE;
}

/* Find the next file or directory in a directory listing.  Should be called
   after doing a dfs_dir_findfirst. */
static int fat64_dir_findnext(char *buf)
{
    if(!valid_dir)
    {
        /* No file found */
        return FLAGS_EOF;
    }

    int ret = fat_readdir(&next_entry);
    if (ret != 1) {
        valid_dir = 0;
        return FLAGS_EOF;
    }

    if(buf)
    {
        strcpy(buf, next_entry.name);
    }

    return next_entry.directory ? FLAGS_DIR : FLAGS_FILE;
}

/* Check if we have any free file handles, and if we do, try
   to open the file specified.  Supports absolute and relative
   paths */
static int fat64_open(const char * const path)
{
    int handle;

    /* Try to find a free slot */
    fat_file_t *file = find_free_file(&handle);

    if(!file)
    {
        return -1; // DFS_ENOMEM; // FIXME
    }

    /* Try to find file */
    int ret = fat_open(path, NULL, file);

    if(ret != FAT_SUCCESS)
    {
        /* File not found, or other error */
        return -ret;
    }

    return handle;
}

/* Close an already open file handle.  Basically just frees up the
   file structure. */
static int fat64_close(uint32_t handle)
{
    fat_file_t *file = find_open_file(handle);

    if(!file)
    {
        return -1; // DFS_EBADHANDLE; // FIXME
    }

    memset(file, 0, sizeof(fat_file_t));
    open_files[handle].open = 0;

    return FAT_SUCCESS;
}

static int fat64_seek(uint32_t handle, int offset, int origin)
{
    int ret;
    fat_file_t *file = find_open_file(handle);

    if(!file)
    {
        return -1; // DFS_EBADHANDLE; FIXME
    }

    ret = fat_lseek(file, offset, origin);
    if (ret != FAT_SUCCESS)
        return -ret; // FIXME

    return FAT_SUCCESS;
}

static int fat64_tell(uint32_t handle)
{
    /* The good thing is that the location is always in the file structure */
    fat_file_t *file = find_open_file(handle);

    if(!file)
    {
        return -1; // DFS_EBADHANDLE; FIXME
    }

    return fat_tell(file);
}

static int fat64_read(void * const buf, int len, uint32_t handle)
{
    /* This is where we do all the work */
    fat_file_t *file = find_open_file(handle);

    if(!file)
    {
        return -1; // DFS_EBADHANDLE; FIXME
    }

    /* What are they doing? */
    if(!buf)
    {
        return -1; // DFS_EBADINPUT; FIXME
    }

    return fat_read(file, buf, len);
}

static int dfs_size(uint32_t handle)
{
    fat_file_t *file = find_open_file(handle);

    if(!file)
    {
        /* Will still count as EOF */
        return -1; // DFS_EBADHANDLE; FIXME
    }

    return file->de.size;
}

static void *__open( char *name, int flags )
{
    /* We disregard flags here */
    return (void *)fat64_open( name );
}

// TODO: use fuse's version
static int __fstat( void *file, struct stat *st )
{
    st->st_dev = 0;
    st->st_ino = 0;
    st->st_mode = S_IFREG;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = dfs_size( (uint32_t)file );
    st->st_atime = 0;
    st->st_mtime = 0;
    st->st_ctime = 0;
    st->st_blksize = 0;
    st->st_blocks = 0;
    //st->st_attr = S_IAREAD | S_IAREAD;

    return 0;
}

static int __lseek( void *file, int ptr, int dir )
{
    fat64_seek( (uint32_t)file, ptr, dir );

    return fat64_tell( (uint32_t)file );
}

static int __read( void *file, uint8_t *ptr, int len )
{
    return fat64_read( ptr, len, (uint32_t)file );
}

static int __close( void *file )
{
    return fat64_close( (uint32_t)file );
}

#ifndef LINUX
static int __findfirst( char *path, dir_t *dir )
{
    if( !path || !dir ) { return -1; }

    /* Grab first entry, return if bad */
    int flags = fat64_dir_findfirst( path, dir->d_name );
    if( flags < 0 ) { return -1; }

    if( flags == FLAGS_FILE )
    {
        dir->d_type = DT_REG;
    }
    else if( flags == FLAGS_DIR )
    {
        dir->d_type = DT_DIR;
    }
    else
    {
        /* Unknown type */
        return -1;
    }

    /* Success */
    return 0;
}

int __findnext( dir_t *dir )
{
    if( !dir ) { return -1; }

    /* Grab first entry, return if bad */
    int flags = fat64_dir_findnext( dir->d_name );
    if( flags < 0 ) { return -1; }

    if( flags == FLAGS_FILE )
    {
        dir->d_type = DT_REG;
    }
    else if( flags == FLAGS_DIR )
    {
        dir->d_type = DT_DIR;
    }
    else
    {
        /* Unknown type */
        return -1;
    }

    /* Success */
    return 0;
}

/* The following section of code is for bridging into newlib's filesystem hooks to allow posix access to libdragon filesystem */
static filesystem_t fat64_fs = {
    __open,
    __fstat,
    __lseek,
    __read,
    0,
    __close,
    0,
    __findfirst,
    __findnext
};

/* Initialize the filesystem.  */
int fat64_init(void)
{
    int ret = fat_init();

    if( ret != FAT_SUCCESS )
    {
        /* Failed, return so */
        return ret;
    }

    /* Succeeded, push our filesystem into newlib */
    attach_filesystem( "cf:/", &fat64_fs );

    return FAT_SUCCESS;
}
#endif

// debugging
#ifdef LINUX

#include <err.h>

int main(int argc, char **argv) {
    int ret, i;
    fat_dirent rde;
    char buf[1024];

    if (argc < 2) {
        printf("Usage: %s <file_system.img>\n", argv[0]);
        // printf("    reads the root directory in debug mode\n"); FIXME
        return 1;
    }

    fat_disk_open(argv[1]);

    ret = fat_init();
    if (ret != 0)
        errx(1, "%s", message1);

    // fat_debug_readdir(fat_fs.root_cluster);

    /*
    // test recurse on file
    ret = fat_recurse_path("/d1/d2/../d2/b", &rde, NULL, TYPE_FILE);
    printf("return %d\nname %s\n", ret, rde.name);
    */

    /*
    // open multiple files
    ret = fat64_open("/d1/d2/b");
    ret = fat64_close(ret);
    ret = fat64_open("/d1/d2/b");
    printf("ret %d\n", ret);
    */

    /*
    // test recurse on dir
    ret = fat_recurse_path("/d1/", &rde, NULL, TYPE_DIR);
    fat_readdir(&rde);
    printf("return %d\nname %s\n", ret, rde.name);
    */

    /*
    // findfirst / findnext
    fat64_dir_findfirst("/d1/d2", buf);
    printf("first: %s\n", buf);

    while (fat64_dir_findnext(buf) != FLAGS_EOF)
        printf("next: %s\n", buf);
        */

    // open/read/seek
    int fd = fat64_open("/d1/dir.c");
    if (fd < 0)
        abort();

    ret = fat64_read(buf, 30, fd);
    if (ret != 30)
        abort();

    puts(buf);

    // first cluster still
    ret = fat64_seek(fd, 3000, SEEK_SET);
    if (ret != 0) {
        printf("%d\n", ret);
        abort();
    }

    ret = fat64_read(buf, 30, fd);
    if (ret != 30)
        abort();
    puts(buf);

    // relative seek into third cluster (to 3030 + 9000 = 12030)
    ret = fat64_seek(fd, 9000, SEEK_CUR);
    if (ret != 0) {
        printf("%d\n", ret);
        abort();
    }

    ret = fat64_read(buf, 30, fd);
    if (ret != 30)
        abort();
    puts(buf);

    return 0;
}

#endif
