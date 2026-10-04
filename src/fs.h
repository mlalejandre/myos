#ifndef MYOS_FS_H
#define MYOS_FS_H

#include <stdint.h>

#define FS_MAX_FILES 32
#define FS_NAME_MAX  48
#define FS_DATA_MAX  4096

struct vfs_file {
    char     name[FS_NAME_MAX];
    uint8_t  used;
    uint8_t  dirty;   /* 1 = modificado en memoria, pendiente de escribir en disco */
    uint32_t size;
    char     data[FS_DATA_MAX];
};

void vfs_init(void);
int  vfs_create(const char *name, const char *initial_data);
int  vfs_write(const char *name, const char *data, uint32_t len);
int  vfs_read(const char *name, char *buf_out, uint32_t max_len);
int  vfs_delete(const char *name);
void vfs_list(void);
int  vfs_format_list(char *out_buf, uint32_t max);
int  vfs_sync(void);
void vfs_format(void);

#endif
