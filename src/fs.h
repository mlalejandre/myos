#ifndef MYOS_FS_H
#define MYOS_FS_H

#include <stdint.h>

#define FS_MAX_FILES 32
#define FS_NAME_MAX  48
#define FS_DATA_MAX  4096

struct vfs_file {
    char     name[FS_NAME_MAX]; /* 48 bytes (offset 0..47) */
    uint32_t size;              /* 4 bytes (offset 48..51, divisible por 4 y 8) */
    uint8_t  used;              /* 1 byte (offset 52) */
    uint8_t  dirty;             /* 1 byte (offset 53) */
    uint8_t  pad[10];           /* 10 bytes (offset 54..63) -> cabecera = 64 B */
    char     data[FS_DATA_MAX]; /* 4096 bytes (offset 64 -> 100% alineado a 64 B) */
} __attribute__((aligned(64)));

void vfs_init(void);
int  vfs_create(const char *name, const char *initial_data);
int  vfs_write(const char *name, const char *data, uint32_t len);
int  vfs_read(const char *name, char *buf_out, uint32_t max_len);
int  vfs_delete(const char *name);
void vfs_list(void);
int  vfs_format_list(char *out_buf, uint32_t max);
int  vfs_sync(void);
void vfs_format(void);
void vfs_tree(char *out_buf, uint32_t max);
void vfs_get_stats(uint32_t *files_used, uint32_t *bytes_used, uint32_t *dirty_count);

/* Checkpoints bare-metal en virtio-blk (LBA 4096) */
int  vfs_checkpoint_save(void);
int  vfs_checkpoint_restore(void);

#endif
