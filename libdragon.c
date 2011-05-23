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

/* Directory walking flags */
enum
{
    WALK_CHDIR,
    WALK_OPEN
};

/* Directory walking return flags */
enum
{
    TYPE_ANY,
    TYPE_FILE,
    TYPE_DIR
};

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

/* Parse out the next token in a path delimited by '\' */
static char *get_next_token(char *path, char *token)
{
    if(!path)
    {
        /* Can't do shit */
        return 0;
    }

    if(path[0] != 0)
    {
        /* We have at least one character in our owth */
        if(path[0] == '/')
        {
            /* Root of path indicator */
            if(token)
            {
                /* Just return the root indicator */
                token[0] = '/';
                token[1] = 0;
            }

            if(*(path + 1) == 0)
            {
                /* Don't return anything if we are at the end */
                return 0;
            }

            /* Don't iterate over this same thing next time */
            return path + 1;
        }
        else
        {
            /* Keep track of copied token so far to not buffer overflow */
            int copied = 0;

            while(path[0] != '/')
            {
                if(path[0] == 0)
                {
                    /* Hit the end */
                    if(token)
                    {
                        /* Cap off the end */
                        token[0] = 0;
                    }

                    /* Nothing comes after this string */
                    return 0;
                }

                /* Only copy over if we need to */
                if(token && copied < MAX_FILENAME_LEN)
                {
                    token[0] = path[0];
                    token++;
                    copied++;
                }

                /* Next entry */
                path++;
            }

            if(token)
            {
                /* Cap off the end */
                token[0] = 0;
            }

            /* Only way we can be here is if the current character is '\' */
            if(*(path + 1) == 0)
            {
                return 0;
            }

            return path + 1;
        }
    }
    else
    {
        if(token)
        {
            /* No token */
            token[0] = 0;
        }

        /* Can't return a shorter path, there was none! */
        return 0;
    }
}

/* Walk a path string, either changing directories or finding the right path

   If mode is WALK_CHDIR, the result of this function is entering into the new
   directory on success, or the old directory being returned on failure.

   If mode is WALK_OPEN, the result of this function is the directory remains
   unchanged and a pointer to the directory entry for the requested file or
   directory is returned.  If it is a file, the directory entry for the file
   itself is returned.  If it is a directory, the directory entry of the first
   file or directory inside that directory is returned.

   The type specifier allows a person to specify that only a directory or file
   should be returned.  This works for WALK_OPEN only. */
static int recurse_path(const char * const path, int mode, fat_dirent *dirent, int type)
{
    int ret = FAT_SUCCESS;
    char token[MAX_FILENAME_LEN+1];
    char *cur_path = (char *)path;
    // uint32_t dir_loc = directory_top;
    int last_type = TYPE_ANY;
    int ignore = 1; // Do not, by default, read again during the first while

    fat_dirent dir_stack[MAX_DIRECTORY_DEPTH];
    fat_root_dirent(&dir_stack[0]);
    int depth = 1;

#define PUSH(DE)                    \
    if(depth < MAX_DIRECTORY_DEPTH) \
    {                               \
        dir_stack[depth] = (DE);    \
        depth++;                    \
    }
#define POP() if(depth > 1) { --depth; }
#define PEEK() dir_stack[depth-1]

    /* Save directory stack */
    // memcpy(dir_stack, directories, sizeof(uint32_t) * MAX_DIRECTORY_DEPTH);

    /* Grab first token, make sure it isn't root */
    cur_path = get_next_token(cur_path, token);

    if(strcmp(token, "/") == 0)
    {
        /* Ensure that we remember this as a directory */
        last_type = TYPE_DIR;

        /* We need to read through the first while loop */
        ignore = 0;
    }

    /* Loop through the rest */
    while(cur_path || ignore)
    {
        /* Grab out the next token */
        if(!ignore) { cur_path = get_next_token(cur_path, token); }
        ignore = 0;

        if(strcmp(token, "/") == 0 ||
           strcmp(token, ".") == 0)
        {
            /* Ignore, this is current directory */
            last_type = TYPE_DIR;
        }
        else if(strcmp(token, "..") == 0)
        {
            /* Up one directory */
            POP();

            last_type = TYPE_DIR;
        }
        else
        {
            /* Find directory entry, push */
            fat_dirent tmp_node, tmp_dir;

            tmp_dir = PEEK();

            ret = fat_find_create(token, &tmp_dir, &tmp_node, 0, 0);

            if(ret == FAT_SUCCESS)
            {
                /* Grab node, make sure it is a directory, push subdirectory, try again! */
                fat_dirent node;

                if (tmp_node.directory)
                {
                    /* Push subdirectory onto stack and loop */
                    fat_sub_dirent(tmp_node.start_cluster, &node);
                    PUSH(node);
                    last_type = TYPE_DIR;
                }
                else
                {
                    if(mode == WALK_CHDIR)
                    {
                        /* Not found, this is a file */
                        ret = FAT_NOTFOUND;
                        break;
                    }
                    else
                    {
                        last_type = TYPE_FILE;

                        /* Only count if this is the last thing we are doing */
                        if(!cur_path)
                        {
                            /* Push file entry onto stack in preparation of a return */
                            PUSH(tmp_node);
                        }
                        else
                        {
                            /* Not found, this is a file */
                            ret = FAT_NOTFOUND;
                            break;
                        }
                    }
                }
            }
            else
            {
                /* Not found! */
                ret = FAT_NOTFOUND;
                break;
            }
        }
    }

    if(type != TYPE_ANY && type != last_type)
    {
        /* Found an entry, but it was the wrong type! */
        ret = FAT_NOTFOUND;
    }

    if(mode == WALK_OPEN)
    {
        /* Must return the node found if we found one */
        if(ret == FAT_SUCCESS && dirent)
        {
            *dirent = PEEK();
        }
    }

    if(mode == WALK_OPEN || ret != FAT_SUCCESS)
    {
        /* Restore stack */
        // directory_top = dir_loc;
        // memcpy(directories, dir_stack, sizeof(uint32_t) * MAX_DIRECTORY_DEPTH);
    }

    return ret;
}

/* Find the first file or directory in a directory listing.  Supports absolute
   and relative.  If the path is invalid, returns a negative DFS_errno.  If
   a file or directory is found, returns the flags of the entry and copies the
   name into buf. */
static int fat64_dir_findfirst(const char * const path, char *buf)
{
    int ret = recurse_path(path, WALK_OPEN, &next_entry, TYPE_DIR);

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
    fat_dirent dirent;
    int ret = recurse_path(path, WALK_OPEN, &dirent, TYPE_FILE);

    if(ret != FAT_SUCCESS)
    {
        /* File not found, or other error */
        return -ret;
    }

    file->de = dirent;
    file->cluster = dirent.start_cluster;
    file->sector = 0;
    file->position = 0;
    // file->size = get_size(&t_node); // FIXME ?

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
    fat_file_t *file = find_open_file(handle);
    uint32_t position;

    if(!file)
    {
        return -1; // DFS_EBADHANDLE; FIXME
    }

    switch(origin)
    {
        case SEEK_SET:
            /* From the beginning */
            if(offset < 0)
            {
                position = 0;
            }
            else
            {
                position = offset;
            }

            break;
        case SEEK_CUR:
        {
            /* From the current position */
            position = file->position + offset;

            if(position < 0)
            {
                position = 0;
            }

            break;
        }
        case SEEK_END:
        {
            /* From the end of the file */
            position = (int)file->de.size + offset;

            if(position < 0)
            {
                position = 0;
            }

            break;
        }
        default:
            /* Impossible */
            return -1; // DFS_EBADINPUT; FIXME
    }

    /* Lets get some bounds checking */
    if(position > file->de.size)
    {
        position = file->de.size;
    }

    int ret = fat_seek(file, position);
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

    return file->position;
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
    /* Always want a consistent interface */
    // dfs_chdir("/");

    /* We disregard flags here */
    return (void *)fat64_open( name );
}

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
    ret = recurse_path("/d1/d2/../d2/b", WALK_OPEN, &rde, TYPE_FILE);
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
    ret = recurse_path("/d1/", WALK_OPEN, &rde, TYPE_DIR);
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
