#include <stdint.h>

#include "console.h"
#include "fs.h"
#include "mem.h"

static struct vfs_file files[FS_MAX_FILES];
static int fs_initialized = 0;

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void vfs_init(void)
{
    if (fs_initialized) return;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].name[0] = '\0';
    }

    fs_initialized = 1;

    /* Archivos iniciales del sistema */
    vfs_create("/etc/hostname", "myos-node1\n");
    vfs_create("/etc/os-release", "NAME=MYOS\nVERSION=0.1-experimental\nARCH=x86_64\nAI_LINK=nail-35b\n");
    vfs_create("/notes.txt", "MYOS Objetivo: Proporcionar a una IA un entorno informatico propio y modificable en bare-metal.\n");
    vfs_create("/sys/status.txt", "KERNEL: x86_64 | VIRTIO: OK | NETWORK: 10.0.2.15 | AI: ONLINE\n");
    vfs_create("/src/kernel.c", "/* MYOS Kernel Entry */\nvoid kernel_main(void) {\n  serial_init();\n  pci_scan();\n  vfs_init();\n  shell_run();\n}\n");
    vfs_create("/src/pci.h", "/* PCI Subsystem */\nuint16_t pci_vendor_id(uint8_t b, uint8_t s, uint8_t f);\nuint32_t pci_bar0(uint8_t b, uint8_t s, uint8_t f);\n");
    vfs_create("/src/virtio.h", "/* VirtIO-NET Driver */\nint virtio_net_send(const void *f, uint16_t len);\nint virtio_net_poll(void *out, uint16_t max, uint16_t *len);\n");
    vfs_create("/src/mem.h", "/* Dynamic Heap Allocator */\nvoid *kmalloc(size_t size);\nvoid kfree(void *ptr);\nvoid kheap_stats(size_t *used, size_t *free_b);\n");
}

int vfs_create(const char *name, const char *initial_data)
{
    if (!fs_initialized) vfs_init();
    if (!name || name[0] == '\0') return -1;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            return vfs_write(name, initial_data, str_len(initial_data));
        }
    }

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) {
            files[i].used = 1;
            str_copy(files[i].name, name, FS_NAME_MAX);
            files[i].size = 0;
            if (initial_data) {
                uint32_t len = str_len(initial_data);
                if (len > FS_DATA_MAX - 1) len = FS_DATA_MAX - 1;
                memcpy(files[i].data, initial_data, len);
                files[i].data[len] = '\0';
                files[i].size = len;
            }
            return 0;
        }
    }

    return -1;
}

int vfs_write(const char *name, const char *data, uint32_t len)
{
    if (!fs_initialized) vfs_init();

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            if (len > FS_DATA_MAX - 1) len = FS_DATA_MAX - 1;
            if (data && len > 0) {
                memcpy(files[i].data, data, len);
            }
            files[i].data[len] = '\0';
            files[i].size = len;
            return (int)len;
        }
    }

    return vfs_create(name, data);
}

int vfs_read(const char *name, char *buf_out, uint32_t max_len)
{
    if (!fs_initialized) vfs_init();
    if (!name || !buf_out || max_len == 0) return -1;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            uint32_t to_copy = files[i].size;
            if (to_copy >= max_len) to_copy = max_len - 1;
            memcpy(buf_out, files[i].data, to_copy);
            buf_out[to_copy] = '\0';
            return (int)to_copy;
        }
    }

    return -1;
}

int vfs_delete(const char *name)
{
    if (!fs_initialized) vfs_init();

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            files[i].used = 0;
            files[i].size = 0;
            files[i].name[0] = '\0';
            return 0;
        }
    }
    return -1;
}

void vfs_list(void)
{
    if (!fs_initialized) vfs_init();

    kprint("\nARCHIVOS EN RamFS (/):\n");
    kprint("----------------------------------------\n");
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            kprint("  ");
            kprint(files[i].name);
            uint32_t nl = str_len(files[i].name);
            for (uint32_t s = nl; s < 28; ++s) kputc(' ');
            kprint_dec(files[i].size);
            kprint(" bytes\n");
            count++;
        }
    }
    if (count == 0) {
        kprint("  (sistema de archivos vacio)\n");
    }
    kprint("----------------------------------------\n");
    kprint("Total: ");
    kprint_dec((uint32_t)count);
    kprint(" archivos.\n\n");
}

int vfs_format_list(char *out_buf, uint32_t max)
{
    if (!fs_initialized) vfs_init();
    uint32_t pos = 0;
    const char *hdr = "Archivos en RamFS: ";
    while (*hdr && pos < max - 1) out_buf[pos++] = *hdr++;

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            if (count > 0 && pos < max - 2) {
                out_buf[pos++] = ',';
                out_buf[pos++] = ' ';
            }
            const char *n = files[i].name;
            while (*n && pos < max - 1) out_buf[pos++] = *n++;
            count++;
        }
    }
    if (count == 0 && pos < max - 8) {
        const char *empty = "(vacio)";
        while (*empty && pos < max - 1) out_buf[pos++] = *empty++;
    }
    out_buf[pos] = '\0';
    return (int)pos;
}
