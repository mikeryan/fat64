#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "fs.h"
#include "common.h"

#define MAX_DIRECTORY_DEPTH     16
#define MAX_FILENAME_LEN        255

// sector read from file
unsigned char file_buffer[512];
uint32_t file_buffer_sector = 0;

// helper functions
static char *get_next_token(char *path, char *token);
static int _fat_load_file_sector(fat_file_t *file);

/**
 * open a file a la fopen, with full path
 *
 * flags:
 *      c   create the file/directory if it doesn't exist
 *      d   used with c, creates a directory instead of a file
 *
 * flags MAY be NULL, in which case no flags are assumed
 *
 * Return:
 *   FAT_SUCCESS    success
 *   FAT_NOTFOUND   file/dir not found and create flag not set
 *   FAT_NOSPACE    file system full
 *   FAT_INCONSISTENT fs needs to be checked
 */
int fat_open(const char *filename, char *flags, fat_file_t *file) {
    int dir, create, ret, ret_type;
    fat_dirent result_de;

    if (flags == NULL) flags = "";
    dir = strchr(flags, 'd') != NULL;
    create = strchr(flags, 'c') != NULL;

    ret = fat_recurse_path(filename, &result_de, &ret_type, TYPE_ANY);

    // attempt to create the file if it doesn't exist
    if (ret != FAT_SUCCESS) {
        char *last_slash;
        char containing_dir[MAX_FILENAME_LEN];
        ptrdiff_t len;
        fat_dirent folder_de;

        // some kind of error, can't do anything here
        if (ret != FAT_NOTFOUND) return ret;

        // don't want to create it, so give up too
        if (!create) return FAT_NOTFOUND;

        //
        // we're going to try to create it
        //

        // no slashes? can't find containing dir either: give up
        last_slash = strrchr(filename, '/');
        if (last_slash == NULL) return FAT_NOTFOUND;
        len = last_slash - filename;

        // containing dir name
        memcpy(containing_dir, filename, len);
        containing_dir[len] = 0;

        ret = fat_recurse_path(containing_dir, &folder_de, &ret_type, TYPE_DIR);
        if (ret != FAT_SUCCESS) return ret;

        // now we have the containing dir, so use that in fat_find_create
        ret = fat_find_create(last_slash + 1, &folder_de, &result_de, dir, 1);
        if (ret != FAT_SUCCESS) return ret;
    }

    file->de = result_de;
    file->dir = ret_type == TYPE_DIR;

    file->cluster = result_de.start_cluster;
    file->sector = 0;
    file->offset = 0;
    file->position = 0;

    return FAT_SUCCESS;
}

/**
 * Open a file from a dirent. Assumes you've found the file you desire using
 * fat_find_create.
 */
void fat_open_from_dirent(fat_file_t *file, fat_dirent *de) {
    file->de = *de;
    file->dir = 0; // FIXME ?

    file->cluster = de->start_cluster;
    file->sector = 0;
    file->offset = 0;
    file->position = 0;
}

/**
 * Read len bytes out of file into buf.
 *
 * Returns the number of bytes read.
 * If return value == 0, file is at EOF.
 *
 * Return of -1 indicates error.
 */
int32_t fat_read(fat_file_t *file, unsigned char *buf, int32_t len) {
    uint32_t bytes_read = 0;
    int ret;

    if (len < 0)
        return -1;

    if (file->position + len > file->de.size)
        len = file->de.size - file->position;

    while (bytes_read < len) {
        int bytes_left;

        ret = _fat_load_file_sector(file);
        // FIXME: fs_error() function
        if (ret != FAT_SUCCESS)
            return -1;

        bytes_left = 512 - file->offset;
        if (bytes_read + bytes_left > len)
            bytes_left = len - bytes_read;

        memcpy(buf + bytes_read, file_buffer + file->offset, bytes_left);
        bytes_read += bytes_left;
        file->position += bytes_left;

        file->offset += bytes_left;
    }

    return bytes_read;
}

/**
 * Seek to absolute position in file.
 * Returns:
 *  FAT_SUCCESS on success
 *  FAT_INCONSISTENT if the file system needs to be checked
 */
static int _fat_seek(fat_file_t *file, uint32_t position) {
    uint32_t bytes_per_clus = fat_fs.sect_per_clus * 512;
    uint32_t seek_left = position;

    // trunc position
    if (position > file->de.size) position = file->de.size;

    file->sector = 0;

    uint32_t cluster = file->de.start_cluster;

    while (seek_left > 512) {
        cluster = fat_get_fat(cluster);
        if (cluster >= 0x0ffffff8)
            return FAT_INCONSISTENT;

        // found the right cluster, so find the right sector and offset
        if (seek_left < bytes_per_clus) {
            file->cluster = cluster;
            while (seek_left >= 512) {
                seek_left -= 512;
                ++file->sector;
            }
        }

        // more clusters to go
        else
            seek_left -= bytes_per_clus;
    }

    file->cluster = cluster;
    file->offset = seek_left;
    file->position = position;
    return FAT_SUCCESS;
}

/**
 * Seek a la lseek
 *
 * Returns
 *  FAT_SUCCESS on success
 *  FAT_BADINPUT if whence is not one of SEEK_SET, SEEK_CUR, or SEEK_END
 *  FAT_INCONSISTENT if the file system needs to be checked
 */
int fat_lseek(fat_file_t *file, off_t offset, int whence) {
    off_t position;

    switch(whence)
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
            return FAT_BADINPUT;
    }

    /* Lets get some bounds checking */
    if(position > file->de.size)
    {
        position = file->de.size;
    }

    return _fat_seek(file, position);
}

/**
 * Return the file's position.
 */
off_t fat_tell(fat_file_t *file) {
    return file->position;
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

/* Walk a path string recursively

   The result of this function is that the fat_dirent argument is populated with
   the result of the recurseion.  If it is a file, the directory entry for the
   file itself is returned.  If it is a directory, the directory entry of the
   first file or directory inside that directory is returned.

   The type specifier allows a person to specify that only a directory or file
   should be returned. */
int fat_recurse_path(const char * const path, fat_dirent *dirent, int *ret_type, int type)
{
    int ret = FAT_SUCCESS;
    char token[MAX_FILENAME_LEN+1];
    char *cur_path = (char *)path;
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
                        return FAT_NOTFOUND;
                    }
                }
            }
            else
            {
                /* Not found or some error */
                return ret;
            }
        }
    }

    if(type != TYPE_ANY && type != last_type)
    {
        /* Found an entry, but it was the wrong type! */
        return FAT_NOTFOUND;
    }

    /* Must return the node found if we found one */
    if(ret == FAT_SUCCESS && dirent)
    {
        *dirent = PEEK();

        if (ret_type) *ret_type = last_type;
    }

    return ret;
}

// load the current sector from the file
// cut & paste code from _dir_load_sector :(
//
// returns:
//  FAT_SUCCESS         success
//  FAT_INCONSISTENT    fs inconsistent
static int _fat_load_file_sector(fat_file_t *file) {
    uint32_t sector;
    uint32_t fat_entry;

    if (file->offset == 512) {
        if (file->sector + 1 == fat_fs.sect_per_clus) {
            // look up the cluster number in the FAT
            fat_entry = fat_get_fat(file->cluster);
            if (fat_entry >= 0x0ffffff8) // last cluster
                return FAT_INCONSISTENT;

            file->cluster = fat_entry;
            file->sector = 0;
            file->offset = 0;
        }
        else {
            ++file->sector;
            file->offset = 0;
        }
    }

    // sector may or may not have changed, but buffering makes this efficient
    sector = CLUSTER_TO_SECTOR(file->cluster) + file->sector;

    // TODO dirty file cluster?
    if (file_buffer_sector != sector) {
        cfReadSector(file_buffer, sector);
        file_buffer_sector = sector;
    }

    return FAT_SUCCESS;
}
