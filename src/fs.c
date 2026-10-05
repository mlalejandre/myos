#include "mutex.h"
#include <stdint.h>

#include "console.h"
#include "fs.h"
#include "mem.h"
#include "virtio_blk.h"

#define FS_SUPER_LBA         1024
#define FS_DATA_LBA          1025
#define FS_SECTORS_PER_FILE  9

struct fs_superblock {
    char     magic[8];       /* "MYOSFS01" */
    uint32_t version;
    uint32_t num_files;
    uint32_t max_files;
    uint8_t  pad[492];
};

struct fs_disk_entry {
    char     name[FS_NAME_MAX]; /* 48 */
    uint32_t size;              /* 4 */
    uint8_t  used;              /* 1 */
    uint8_t  pad[11];           /* 11 -> 64 bytes encabezado */
    char     data[FS_DATA_MAX]; /* 4096 */
};

static struct vfs_file files[FS_MAX_FILES];
static struct kmutex vfs_mutex;
static int fs_initialized = 0;

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static const char *fs_strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return 0;
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

static int sync_file_to_disk(int i)
{
    uint8_t sec_buf[512];
    struct fs_disk_entry entry;
    memset(&entry, 0, sizeof(entry));

    memcpy(entry.name, files[i].name, FS_NAME_MAX);
    entry.size = files[i].size;
    entry.used = files[i].used;
    memcpy(entry.data, files[i].data, FS_DATA_MAX);

    uint64_t start_lba = FS_DATA_LBA + (uint64_t)i * FS_SECTORS_PER_FILE;
    const uint8_t *raw = (const uint8_t *)&entry;

    for (int s = 0; s < FS_SECTORS_PER_FILE; ++s) {
        memset(sec_buf, 0, sizeof(sec_buf));
        uint32_t offset = (uint32_t)s * 512;
        uint32_t to_copy = 512;
        if (offset + to_copy > sizeof(entry)) {
            to_copy = sizeof(entry) > offset ? sizeof(entry) - offset : 0;
        }
        if (to_copy > 0) {
            memcpy(sec_buf, raw + offset, to_copy);
        }
        if (virtio_blk_write(start_lba + s, sec_buf) != 0) {
            return -1;
        }
    }
    files[i].dirty = 0;
    return 0;
}

static int load_file_from_disk(int i)
{
    uint8_t sec_buf[512];
    struct fs_disk_entry entry;
    memset(&entry, 0, sizeof(entry));

    uint64_t start_lba = FS_DATA_LBA + (uint64_t)i * FS_SECTORS_PER_FILE;
    uint8_t *raw = (uint8_t *)&entry;

    for (int s = 0; s < FS_SECTORS_PER_FILE; ++s) {
        if (virtio_blk_read(start_lba + s, sec_buf) != 0) {
            return -1;
        }
        uint32_t offset = (uint32_t)s * 512;
        uint32_t to_copy = 512;
        if (offset + to_copy > sizeof(entry)) {
            to_copy = sizeof(entry) > offset ? sizeof(entry) - offset : 0;
        }
        if (to_copy > 0) {
            memcpy(raw + offset, sec_buf, to_copy);
        }
    }

    /* No aceptar estructuras de disco que puedan provocar lecturas/escrituras
       fuera de los limites del VFS o cadenas no terminadas. */
    if (entry.used > 1) {
        return -1;
    }
    if (entry.size >= FS_DATA_MAX) {
        return -1;
    }
    if (entry.used) {
        int name_terminated = 0;
        for (uint32_t n = 0; n < FS_NAME_MAX; ++n) {
            if (entry.name[n] == '\0') {
                name_terminated = 1;
                break;
            }
        }
        if (!name_terminated || entry.name[0] == '\0') {
            return -1;
        }
        if (entry.size > 0 && entry.data[entry.size] != '\0') {
            return -1;
        }
    }

    memcpy(files[i].name, entry.name, FS_NAME_MAX);
    files[i].size = entry.size;
    files[i].used = entry.used;
    files[i].dirty = 0;
    memcpy(files[i].data, entry.data, FS_DATA_MAX);
    return 0;
}

static int vfs_sync_unlocked(void)
{
    struct fs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, "SOMAFS01", 8);
    sb.version = 1;
    sb.max_files = FS_MAX_FILES;

    uint32_t used_count = 0;
    int any_dirty = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            if (files[i].size >= FS_DATA_MAX) {
                kprint("VFS: entrada invalida en memoria; sync abortada.\n");
                return -1;
            }
            used_count++;
        }
        if (files[i].dirty) {
            any_dirty = 1;
            if (sync_file_to_disk(i) != 0) {
                kprint("VFS: error de I/O sincronizando archivo; sync abortada.\n");
                return -1;
            }
        }
    }
    sb.num_files = used_count;

    if (any_dirty) {
        if (virtio_blk_write(FS_SUPER_LBA, &sb) != 0) {
            kprint("VFS: error de I/O escribiendo superbloque.\n");
            return -1;
        }
    }

    return 0;
}

static int vfs_create_internal(const char *name, const char *initial_data)
{
    if (!name || name[0] == '\0') return -1;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            uint32_t len = str_len(initial_data);
            if (len > FS_DATA_MAX - 1) len = FS_DATA_MAX - 1;
            if (initial_data && len > 0) memcpy(files[i].data, initial_data, len);
            files[i].data[len] = '\0';
            files[i].size = len;
            files[i].dirty = 1;
            return 0;
        }
    }

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) {
            files[i].used = 1;
            files[i].dirty = 1;
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

static void vfs_format_unlocked(void)
{
    /* 1. Borrado fisico a bajo nivel de todos los sectores del RamFS en el disco */
    static uint8_t zero_buf[512] __attribute__((aligned(16)));
    memset(zero_buf, 0, sizeof(zero_buf));
    
    uint32_t fs_total_sectors = 1 + FS_MAX_FILES * 9;
    for (uint32_t s = 0; s < fs_total_sectors; ++s) {
        virtio_blk_write(1024 + s, zero_buf);
    }
    virtio_blk_write(4095, zero_buf); /* Limpiar checkpoint */

    /* 2. Limpiar memoria RAM */
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].dirty = 0;
        files[i].name[0] = '\0';
        memset(files[i].data, 0, 4096);
    }

    /* 3. Recrear estado de fabrica puro SOMA */
    vfs_create_internal("/etc/hostname", "soma-node1\n");
    vfs_create_internal("/etc/os-release", "NAME=SOMA\nVERSION=0.2-release\nARCH=x86_64\nSTORAGE=virtio-blk-persistent\nDESCRIPTION=Sistema Operativo Multi-Agente\n");
    vfs_create_internal("/sys/status.txt", "KERNEL: SOMA x86_64 | VIRTIO: OK | NETWORK: 10.0.2.15 | STORAGE: PERSISTENT\n");
    vfs_create_internal("/etc/soma_manifest.txt", "Identity: SOMA AI Kernel (Nail-35b)\nRole: Multi-Agent System Core Intelligence\nStatus: Fully Operational\n");
    vfs_create_internal("/src/kernel.c", "/* SOMA Kernel Entry */\nvoid kernel_main(void) {\n  virtio_blk_init();\n  vfs_init();\n  shell_run();\n}\n");

    /* 4. Forzar guardado al disco recien limpiado */
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) files[i].dirty = 1;
    }
    vfs_sync_unlocked();
}

void vfs_init(void)
{
    if (fs_initialized) return;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].dirty = 0;
        files[i].name[0] = '\0';
    }

    fs_initialized = 1;
    kmutex_init(&vfs_mutex, "vfs_lock");

    struct fs_superblock sb;
    if (virtio_blk_read(FS_SUPER_LBA, &sb) == 0 &&
        (memcmp(sb.magic, "SOMAFS01", 8) == 0 || memcmp(sb.magic, "MYOSFS01", 8) == 0) &&
        sb.version == 1 &&
        sb.max_files == FS_MAX_FILES &&
        sb.num_files <= FS_MAX_FILES) {
        kprint("VFS: Sistema de archivos persistente detectado (MYOSFS01)\n");
        int loaded = 0;
        int invalid = 0;
        for (int i = 0; i < FS_MAX_FILES; ++i) {
            int r = load_file_from_disk(i);
            if (r == 0 && files[i].used) {
                loaded++;
            } else if (r != 0) {
                invalid++;
                files[i].used = 0;
                files[i].size = 0;
                files[i].name[0] = '\0';
            }
        }
        kprint("VFS: ");
        kprint_dec((uint32_t)loaded);
        kprint(" archivos cargados desde virtio-blk.\n");

        /* Migracion automatica de identidad a SOMA 0.2 */
        char check_os[128];
        if (vfs_read("/etc/os-release", check_os, sizeof(check_os)) > 0) {
            if (fs_strstr(check_os, "NAME=MYOS")) {
                vfs_write("/etc/os-release", "NAME=SOMA\nVERSION=0.2-release\nARCH=x86_64\nSTORAGE=virtio-blk-persistent\nDESCRIPTION=Sistema Operativo Multi-Agente\n", 112);
                vfs_write("/etc/hostname", "soma-node1\n", 11);
                vfs_write("/notes.txt", "SOMA: Sistema Operativo Multi-Agente con IA nativa y control directo sobre hardware.\n", 85);
                vfs_write("/sys/status.txt", "KERNEL: SOMA x86_64 | VIRTIO: OK | NETWORK: 10.0.2.15 | STORAGE: PERSISTENT\n", 76);
                kprint("VFS: Inodos de sistema migrados con exito a SOMA 0.2.\n");
            }
        }
        if ((uint32_t)loaded != sb.num_files || invalid != 0) {
            kprint("VFS: AVISO: inconsistencias detectadas; archivos invalidos omitidos.\n");
        }
        return;
    }

    kprint("VFS: Inicializando sistema de archivos en virtio-blk (LBA 1024)...\n");
    vfs_format();
}

static int vfs_create_unlocked(const char *name, const char *initial_data)
{
    if (!fs_initialized) vfs_init();
    int r = vfs_create_internal(name, initial_data);
    if (r == 0 && vfs_sync() != 0) {
        return -1;
    }
    return r;
}

int vfs_write(const char *name, const char *data, uint32_t len)
{
    if (!fs_initialized) vfs_init();
    kmutex_lock(&vfs_mutex);

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            if (len > FS_DATA_MAX - 1) len = FS_DATA_MAX - 1;
            if (data && len > 0) {
                memcpy(files[i].data, data, len);
            }
            files[i].data[len] = '\0';
            files[i].size = len;
            files[i].dirty = 1;
            if (vfs_sync_unlocked() != 0) {
                kmutex_unlock(&vfs_mutex);
                return -1;
            }
            kmutex_unlock(&vfs_mutex);
            return (int)len;
        }
    }

    int r = vfs_create_unlocked(name, data);
    kmutex_unlock(&vfs_mutex);
    return r == 0 ? (int)len : -1;
}

static int vfs_read_unlocked(const char *name, char *buf_out, uint32_t max_len)
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

static int vfs_delete_unlocked(const char *name)
{
    if (!fs_initialized) vfs_init();

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            files[i].used = 0;
            files[i].size = 0;
            files[i].name[0] = '\0';
            files[i].dirty = 1;
            if (vfs_sync() != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static void vfs_list_unlocked(void)
{
    if (!fs_initialized) vfs_init();

    kprint("\nARCHIVOS EN RamFS (Persistente en virtio-blk):\n");
    kprint("--------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            kprint("  ");
            kprint(files[i].name);
            uint32_t nl = str_len(files[i].name);
            for (uint32_t s = nl; s < 30; ++s) kputc(' ');
            kprint_dec(files[i].size);
            kprint(" bytes\n");
            count++;
        }
    }
    if (count == 0) {
        kprint("  (sistema de archivos vacio)\n");
    }
    kprint("--------------------------------------------------\n");
    kprint("Total: ");
    kprint_dec((uint32_t)count);
    kprint(" archivos.\n\n");
}

static int vfs_format_list_unlocked(char *out_buf, uint32_t max)
{
    if (!fs_initialized) vfs_init();
    uint32_t pos = 0;
    const char *hdr = "Archivos persistentes: ";
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


static void vfs_get_stats_unlocked(uint32_t *files_used, uint32_t *bytes_used, uint32_t *dirty_count)
{
    uint32_t f = 0, b = 0, d = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            f++;
            b += files[i].size;
            if (files[i].dirty) d++;
        }
    }
    if (files_used) *files_used = f;
    if (bytes_used) *bytes_used = b;
    if (dirty_count) *dirty_count = d;
}

static void vfs_tree_unlocked(char *out_buf, uint32_t max)
{
    if (!fs_initialized) vfs_init();
    uint32_t pos = 0;
    const char *hdr = "/ (RamFS persistente MYOSFS01)\n";
    kprint(hdr);
    while (*hdr && pos < max - 1) out_buf[pos++] = *hdr++;

    char dirs[16][32];
    int dir_count = 0;
    uint32_t total_bytes = 0;
    int total_files = 0;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) continue;
        total_files++;
        total_bytes += files[i].size;
        const char *fn = files[i].name;
        if (fn[0] == '/') fn++;
        int slash = -1;
        for (int j = 0; fn[j]; ++j) {
            if (fn[j] == '/') { slash = j; break; }
        }
        if (slash > 0) {
            char dname[32];
            int k = 0;
            for (; k < slash && k < 31; ++k) dname[k] = fn[k];
            dname[k] = '\0';
            int exists = 0;
            for (int d = 0; d < dir_count; ++d) {
                if (str_eq(dirs[d], dname)) { exists = 1; break; }
            }
            if (!exists && dir_count < 16) {
                str_copy(dirs[dir_count++], dname, 32);
            }
        }
    }

    for (int d = 0; d < dir_count; ++d) {
        kprint("+-- "); kprint(dirs[d]); kprint("/\n");
        const char *p1 = "+-- "; while (*p1 && pos < max - 1) out_buf[pos++] = *p1++;
        const char *p2 = dirs[d]; while (*p2 && pos < max - 1) out_buf[pos++] = *p2++;
        if (pos < max - 2) { out_buf[pos++] = '/'; out_buf[pos++] = '\n'; }
        uint32_t dlen = str_len(dirs[d]);
        for (int i = 0; i < FS_MAX_FILES; ++i) {
            if (!files[i].used) continue;
            const char *fn = files[i].name;
            if (fn[0] == '/') fn++;
            int match = 1;
            for (uint32_t k = 0; k < dlen; ++k) {
                if (fn[k] != dirs[d][k]) { match = 0; break; }
            }
            if (match && fn[dlen] == '/') {
                kprint("|   +-- "); kprint(fn + dlen + 1); kprint("\n");
                const char *p3 = "|   +-- "; while (*p3 && pos < max - 1) out_buf[pos++] = *p3++;
                const char *p4 = fn + dlen + 1; while (*p4 && pos < max - 1) out_buf[pos++] = *p4++;
                if (pos < max - 1) out_buf[pos++] = '\n';
            }
        }
    }

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) continue;
        const char *fn = files[i].name;
        if (fn[0] == '/') fn++;
        int has_sub = 0;
        for (int j = 0; fn[j]; ++j) {
            if (fn[j] == '/') { has_sub = 1; break; }
        }
        if (!has_sub) {
            kprint("+-- "); kprint(fn); kprint("\n");
            const char *p5 = "+-- "; while (*p5 && pos < max - 1) out_buf[pos++] = *p5++;
            const char *p6 = fn; while (*p6 && pos < max - 1) out_buf[pos++] = *p6++;
            if (pos < max - 1) out_buf[pos++] = '\n';
        }
    }
    out_buf[pos] = '\0';
}

void vfs_get_stats(uint32_t *files_used, uint32_t *bytes_used, uint32_t *dirty_count)
{
    kmutex_lock(&vfs_mutex);
    vfs_get_stats_unlocked(files_used, bytes_used, dirty_count);
    kmutex_unlock(&vfs_mutex);
}

void vfs_tree(char *out_buf, uint32_t max)
{
    kmutex_lock(&vfs_mutex);
    vfs_tree_unlocked(out_buf, max);
    kmutex_unlock(&vfs_mutex);
}

/* ---- Envoltorios con cerrojo (mutex recursivo) ---- */
int vfs_sync(void)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_sync_unlocked();
    kmutex_unlock(&vfs_mutex);
    return r;
}

void vfs_format(void)
{
    kmutex_lock(&vfs_mutex);
    vfs_format_unlocked();
    kmutex_unlock(&vfs_mutex);
}

int vfs_create(const char *name, const char *initial_data)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_create_unlocked(name, initial_data);
    kmutex_unlock(&vfs_mutex);
    return r;
}

int vfs_read(const char *name, char *buf_out, uint32_t max_len)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_read_unlocked(name, buf_out, max_len);
    kmutex_unlock(&vfs_mutex);
    return r;
}

int vfs_delete(const char *name)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_delete_unlocked(name);
    kmutex_unlock(&vfs_mutex);
    return r;
}

void vfs_list(void)
{
    kmutex_lock(&vfs_mutex);
    vfs_list_unlocked();
    kmutex_unlock(&vfs_mutex);
}

int vfs_format_list(char *out_buf, uint32_t max)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_format_list_unlocked(out_buf, max);
    kmutex_unlock(&vfs_mutex);
    return r;
}

#define FS_CHECKPOINT_LBA  4096
#define FS_TOTAL_SECTORS   (1 + FS_MAX_FILES * FS_SECTORS_PER_FILE)

int vfs_checkpoint_save(void)
{
    if (!fs_initialized) vfs_init();
    kmutex_lock(&vfs_mutex);
    vfs_sync_unlocked();

    static uint8_t c_buf[512] __attribute__((aligned(16)));
    memset(c_buf, 0, sizeof(c_buf));
    memcpy(c_buf, "MYOS_CHECKPOINT_V1", 18);
    if (virtio_blk_write(FS_CHECKPOINT_LBA - 1, c_buf) != 0) {
        kmutex_unlock(&vfs_mutex);
        return -1;
    }

    for (uint32_t s = 0; s < FS_TOTAL_SECTORS; ++s) {
        if (virtio_blk_read(FS_SUPER_LBA + s, c_buf) != 0 ||
            virtio_blk_write(FS_CHECKPOINT_LBA + s, c_buf) != 0) {
            kmutex_unlock(&vfs_mutex);
            return -1;
        }
    }

    kmutex_unlock(&vfs_mutex);
    return 0;
}

int vfs_checkpoint_restore(void)
{
    if (!fs_initialized) vfs_init();
    kmutex_lock(&vfs_mutex);

    static uint8_t c_buf[512] __attribute__((aligned(16)));
    if (virtio_blk_read(FS_CHECKPOINT_LBA - 1, c_buf) != 0 ||
        memcmp(c_buf, "MYOS_CHECKPOINT_V1", 18) != 0) {
        kmutex_unlock(&vfs_mutex);
        return -1;
    }

    for (uint32_t s = 0; s < FS_TOTAL_SECTORS; ++s) {
        if (virtio_blk_read(FS_CHECKPOINT_LBA + s, c_buf) != 0 ||
            virtio_blk_write(FS_SUPER_LBA + s, c_buf) != 0) {
            kmutex_unlock(&vfs_mutex);
            return -1;
        }
    }

    /* Recargar archivos a memoria */
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        load_file_from_disk(i);
    }

    kmutex_unlock(&vfs_mutex);
    return 0;
}
