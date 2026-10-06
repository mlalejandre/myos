#include "mutex.h"
#include <stdint.h>
#include "console.h"
#include "fs.h"
#include "mem.h"
#include "virtio_blk.h"
#include "sysinfo.h"
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "rtc.h"
#include "net.h"
#include "io.h"
#include "arch.h"
#include "idt.h"
#include "virtio_net.h"

#define FS_SUPER_LBA         1024
#define FS_DATA_LBA          1025
#define FS_SECTORS_PER_FILE  9
#define FS_CHECKPOINT_LBA    4096
#define FS_TOTAL_SECTORS     (1 + FS_MAX_FILES * FS_SECTORS_PER_FILE)

struct fs_superblock {
    char     magic[8];       /* "SOMAFS01" */
    uint32_t version;
    uint32_t num_files;
    uint32_t max_files;
    uint8_t  pad[492];
} __attribute__((aligned(64)));

struct fs_disk_entry {
    char     name[FS_NAME_MAX]; /* 48 bytes (offset 0..47) */
    uint32_t size;              /* 4 bytes (offset 48..51) */
    uint8_t  used;              /* 1 byte (offset 52) */
    uint8_t  pad[11];           /* 11 bytes (offset 53..63) -> cabecera = 64 B */
    char     data[FS_DATA_MAX]; /* 4096 bytes (offset 64) */
} __attribute__((aligned(64)));

static struct vfs_file files[FS_MAX_FILES] __attribute__((aligned(64)));
static struct kmutex vfs_mutex __attribute__((aligned(16)));
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
        while (*h && *n && (*h == *n)) { h++; n++; }
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
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void fb_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    if (!out || max == 0 || !pos || *pos >= max - 1) return;
    while (*s && *pos + 1 < max) out[(*pos)++] = *s++;
    out[*pos] = '\0';
}

static void fb_put_dec(char *out, uint32_t max, uint32_t *pos, uint64_t v)
{
    char tmp[24]; int n = 0;
    if (v == 0) { if (*pos + 1 < max) { out[(*pos)++] = '0'; out[*pos] = '\0'; } return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && *pos + 1 < max) out[(*pos)++] = tmp[--n];
    out[*pos] = '\0';
}

static int is_ephemeral_path(const char *path)
{
    return (fs_strstr(path, "/sys/") != 0 || fs_strstr(path, "/tmp/") != 0);
}

static int is_proc_path(const char *path)
{
    return (path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' && path[4] == 'c' && (path[5] == '/' || path[5] == '\0'));
}

static int is_dev_path(const char *path)
{
    return (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && (path[4] == '/' || path[4] == '\0'));
}

/* PARCHE 046: rutas que el Agente ReAct no puede escribir ni borrar.
 * /etc/init.sh se ejecuta en cada arranque SIN politica de agente: permitir su
 * escritura (via '>', cp, mv, touch...) era una escalada de privilegios persistente. */
static int agent_path_denied(const char *name)
{
    static const char *const deny_exact[] = {
        "/etc/init.sh", "/etc/soma_manifest.txt", "/etc/history.txt", 0
    };
    struct tcb *t = thread_current();
    if (!t || !t->agent_mode || !name) return 0;

    int deny = (fs_strstr(name, "/agent/") == name);
    for (int i = 0; !deny && deny_exact[i]; ++i) {
        if (str_eq(name, deny_exact[i])) deny = 1;
    }
    if (deny) {
        kprint("\n[SEGURIDAD AGENTE] Escritura denegada en '");
        kprint(name);
        kprint("' (ruta protegida).\n");
    }
    return deny;
}

static uint64_t rng_state = 0x88888888ULL;
static uint64_t xorshift64(void)
{
    if (rng_state == 0) rng_state = arch_cycle_counter();
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}

static int read_proc_file(const char *name, char *buf_out, uint32_t max_len)
{
    uint32_t pos = 0;
    if (str_eq(name, "/proc/version")) {
        fb_puts(buf_out, max_len, &pos, "SOMA version 0.2-release (" MYOS_ARCH_NAME ") gcc-freestanding #1 Oct 2026\n");
        return (int)pos;
    }
    if (str_eq(name, "/proc/uptime")) {
        uint64_t ms = timer_get_uptime_ms();
        fb_put_dec(buf_out, max_len, &pos, ms / 1000ULL);
        fb_puts(buf_out, max_len, &pos, ".");
        uint32_t frac = (uint32_t)(ms % 1000ULL);
        if (frac < 100) fb_puts(buf_out, max_len, &pos, "0");
        if (frac < 10)  fb_puts(buf_out, max_len, &pos, "0");
        fb_put_dec(buf_out, max_len, &pos, frac);
        fb_puts(buf_out, max_len, &pos, " seconds\n");
        return (int)pos;
    }
    if (str_eq(name, "/proc/meminfo")) {
        size_t f_free = 0, f_used = 0, f_total = 0;
        pmm_get_stats(&f_free, &f_used, &f_total);
        size_t h_used = 0, h_free = 0;
        kheap_stats(&h_used, &h_free);
        fb_puts(buf_out, max_len, &pos, "MemTotal:       "); fb_put_dec(buf_out, max_len, &pos, (f_total * 4096ULL) / 1024ULL);
        fb_puts(buf_out, max_len, &pos, " kB\nMemFree:        "); fb_put_dec(buf_out, max_len, &pos, (f_free * 4096ULL) / 1024ULL);
        fb_puts(buf_out, max_len, &pos, " kB\nHeapFree:       "); fb_put_dec(buf_out, max_len, &pos, h_free / 1024ULL);
        fb_puts(buf_out, max_len, &pos, " kB\nHeapUsed:       "); fb_put_dec(buf_out, max_len, &pos, h_used / 1024ULL);
        fb_puts(buf_out, max_len, &pos, " kB\n");
        return (int)pos;
    }
    if (str_eq(name, "/proc/threads")) {
        return thread_format_table(buf_out, max_len);
    }
    if (str_eq(name, "/proc/net/dev")) {
        uint32_t tx_p = 0, rx_p = 0; uint64_t tx_b = 0, rx_b = 0;
        virtio_net_stats(&tx_p, &rx_p, &tx_b, &rx_b);
        fb_puts(buf_out, max_len, &pos, "Interface eth0: rx_bytes="); fb_put_dec(buf_out, max_len, &pos, rx_b);
        fb_puts(buf_out, max_len, &pos, " tx_bytes="); fb_put_dec(buf_out, max_len, &pos, tx_b);
        fb_puts(buf_out, max_len, &pos, "\n");
        return (int)pos;
    }
    if (str_eq(name, "/proc/net/arp")) {
        return arp_format_cache(buf_out, max_len);
    }
    return -1;
}

static int read_dev_file(const char *name, char *buf_out, uint32_t max_len)
{
    if (str_eq(name, "/dev/null")) { buf_out[0] = '\0'; return 0; }
    if (str_eq(name, "/dev/zero")) {
        uint32_t n = max_len > 1 ? max_len - 1 : 0;
        for (uint32_t i = 0; i < n; ++i) buf_out[i] = '\0';
        buf_out[n] = '\0'; return (int)n;
    }
    if (str_eq(name, "/dev/urandom")) {
        uint32_t n = 64; if (n >= max_len) n = max_len - 1;
        const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        for (uint32_t i = 0; i < n; ++i) buf_out[i] = charset[xorshift64() % (sizeof(charset) - 1)];
        buf_out[n] = '\0'; return (int)n;
    }
    if (str_eq(name, "/dev/rtc")) {
        struct rtc_time t;
        rtc_get_datetime(&t);
        uint32_t pos = 0;
        fb_puts(buf_out, max_len, &pos, "RTC: ");
        fb_put_dec(buf_out, max_len, &pos, t.year);
        fb_puts(buf_out, max_len, &pos, "-");
        if (t.month < 10) { fb_puts(buf_out, max_len, &pos, "0"); }
        fb_put_dec(buf_out, max_len, &pos, t.month);
        fb_puts(buf_out, max_len, &pos, "-");
        if (t.day < 10) { fb_puts(buf_out, max_len, &pos, "0"); }
        fb_put_dec(buf_out, max_len, &pos, t.day);
        fb_puts(buf_out, max_len, &pos, " ");
        if (t.hour < 10) { fb_puts(buf_out, max_len, &pos, "0"); }
        fb_put_dec(buf_out, max_len, &pos, t.hour);
        fb_puts(buf_out, max_len, &pos, ":");
        if (t.min < 10) { fb_puts(buf_out, max_len, &pos, "0"); }
        fb_put_dec(buf_out, max_len, &pos, t.min);
        fb_puts(buf_out, max_len, &pos, ":");
        if (t.sec < 10) { fb_puts(buf_out, max_len, &pos, "0"); }
        fb_put_dec(buf_out, max_len, &pos, t.sec);
        fb_puts(buf_out, max_len, &pos, " UTC\n");
        return (int)pos;
    }
    return -1;
}

static int sync_file_to_disk(int i)
{
    uint8_t sec_buf[512] __attribute__((aligned(16)));
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
        if (offset + to_copy > sizeof(entry)) to_copy = sizeof(entry) > offset ? sizeof(entry) - offset : 0;
        if (to_copy > 0) memcpy(sec_buf, raw + offset, to_copy);
        if (virtio_blk_write(start_lba + s, sec_buf) != 0) return -1;
    }
    files[i].dirty = 0;
    return 0;
}

static int load_file_from_disk(int i)
{
    uint8_t sec_buf[512] __attribute__((aligned(16)));
    struct fs_disk_entry entry;
    memset(&entry, 0, sizeof(entry));

    uint64_t start_lba = FS_DATA_LBA + (uint64_t)i * FS_SECTORS_PER_FILE;
    uint8_t *raw = (uint8_t *)&entry;

    for (int s = 0; s < FS_SECTORS_PER_FILE; ++s) {
        if (virtio_blk_read(start_lba + s, sec_buf) != 0) return -1;
        uint32_t offset = (uint32_t)s * 512;
        uint32_t to_copy = 512;
        if (offset + to_copy > sizeof(entry)) to_copy = sizeof(entry) > offset ? sizeof(entry) - offset : 0;
        if (to_copy > 0) memcpy(raw + offset, sec_buf, to_copy);
    }

    if (entry.used > 1 || entry.size >= FS_DATA_MAX) return -1;
    if (entry.used) {
        int name_term = 0;
        for (uint32_t n = 0; n < FS_NAME_MAX; ++n) {
            if (entry.name[n] == '\0') { name_term = 1; break; }
        }
        if (!name_term || entry.name[0] == '\0') return -1;
        if (entry.size > 0 && entry.data[entry.size] != '\0') return -1;
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
            if (files[i].size >= FS_DATA_MAX) return -1;
            used_count++;
        }
        if (files[i].dirty) {
            any_dirty = 1;
            if (sync_file_to_disk(i) != 0) return -1;
        }
    }
    sb.num_files = used_count;
    if (any_dirty) {
        if (virtio_blk_write(FS_SUPER_LBA, &sb) != 0) return -1;
    }
    return 0;
}

static int vfs_create_internal(const char *name, const char *initial_data, uint32_t len)
{
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            if (len > FS_DATA_MAX - 1) return -2;
            if (initial_data && len > 0) memcpy(files[i].data, initial_data, len);
            files[i].data[len] = '\0';
            files[i].size = len;
            files[i].dirty = !is_ephemeral_path(name);
            return 0;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) {
            if (initial_data && len > FS_DATA_MAX - 1) return -2;
            files[i].used = 1;
            files[i].dirty = !is_ephemeral_path(name);
            str_copy(files[i].name, name, FS_NAME_MAX);
            files[i].size = 0;
            if (initial_data) {
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
    static uint8_t zero_buf[512] __attribute__((aligned(16)));
    memset(zero_buf, 0, sizeof(zero_buf));
    
    uint32_t fs_total_sectors = 1 + FS_MAX_FILES * 9;
    for (uint32_t s = 0; s < fs_total_sectors; ++s) {
        virtio_blk_write(1024 + s, zero_buf);
    }
    virtio_blk_write(4095, zero_buf);

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].dirty = 0;
        files[i].name[0] = '\0';
        memset(files[i].data, 0, 4096);
    }

    vfs_create_internal("/etc/hostname", "soma-node1\n", 11);
    vfs_create_internal("/etc/os-release", "NAME=SOMA\nVERSION=0.2-release\nARCH=" MYOS_ARCH_NAME "\nSTORAGE=virtio-blk-persistent\nDESCRIPTION=Sistema Operativo Multi-Agente\n", 112);
    vfs_create_internal("/sys/status.txt", "KERNEL: SOMA " MYOS_ARCH_NAME " | VIRTIO: OK | NETWORK: 10.0.2.15 | STORAGE: PERSISTENT\n", 76);

    const char *init_code = "#!/bin/soma\n# SOMA Init Script\nexport MOTD=\"SOMA Bare-Metal Core Activo\"\necho \"$MOTD ($HOST en $ARCH)\"\n";
    vfs_create_internal("/etc/init.sh", init_code, str_len(init_code));

    vfs_create_internal("/etc/soma_manifest.txt", "Identity: SOMA AI Kernel (Nail-35b)\nRole: Multi-Agent System Core Intelligence\nStatus: Fully Operational\n", 102);
    vfs_create_internal("/src/kernel.c", "/* SOMA Kernel Entry */\nvoid kernel_main(void) {\n  virtio_blk_init();\n  vfs_init();\n  shell_run();\n}\n", 94);

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
        kprint("VFS: Sistema de archivos persistente detectado (SOMAFS01)\n");
        int loaded = 0;
        for (int i = 0; i < FS_MAX_FILES; ++i) {
            int r = load_file_from_disk(i);
            if (r == 0 && files[i].used) loaded++;
        }
        kprint("VFS: "); kprint_dec((uint32_t)loaded); kprint(" archivos cargados desde virtio-blk.\n");
        kprint("VFS: Pseudo-filesystems /proc/ y /dev/ montados en RAM.\n");

        char chk_init[16];
        if (vfs_read("/etc/init.sh", chk_init, sizeof(chk_init)) <= 0) {
            const char *init_code = "#!/bin/soma\n# SOMA Init Script\nexport MOTD=\"SOMA Bare-Metal Core Activo\"\necho \"$MOTD ($HOST en $ARCH)\"\n";
            vfs_write("/etc/init.sh", init_code, str_len(init_code));
        }
        return;
    }

    kprint("VFS: Inicializando sistema de archivos en virtio-blk (LBA 1024)...\n");
    vfs_format();
}

static int vfs_create_unlocked(const char *name, const char *initial_data, uint32_t len)
{
    if (!fs_initialized) vfs_init();
    int r = vfs_create_internal(name, initial_data, len);
    if (r == 0 && !is_ephemeral_path(name) && vfs_sync_unlocked() != 0) {
        return -1;
    }
    return r;
}

int vfs_write(const char *name, const char *data, uint32_t len)
{
    if (!fs_initialized) vfs_init();
    if (!name) return -1;
    if (is_proc_path(name)) return -1;
    if (agent_path_denied(name)) return -1;
    if (str_eq(name, "/dev/null") || str_eq(name, "/dev/zero")) return (int)len;

    kmutex_lock(&vfs_mutex);
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            if (len > FS_DATA_MAX - 1) { kmutex_unlock(&vfs_mutex); return -2; }
            if (data && len > 0) memcpy(files[i].data, data, len);
            files[i].data[len] = '\0';
            files[i].size = len;
            if (!is_ephemeral_path(name)) {
                files[i].dirty = 1;
                if (vfs_sync_unlocked() != 0) { kmutex_unlock(&vfs_mutex); return -1; }
            }
            kmutex_unlock(&vfs_mutex);
            return (int)len;
        }
    }
    int r = vfs_create_unlocked(name, data, len);
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

int vfs_read(const char *name, char *buf_out, uint32_t max_len)
{
    if (!fs_initialized) vfs_init();
    if (!name || !buf_out || max_len == 0) return -1;

    if (is_proc_path(name)) return read_proc_file(name, buf_out, max_len);
    if (is_dev_path(name))  return read_dev_file(name, buf_out, max_len);

    kmutex_lock(&vfs_mutex);
    int r = vfs_read_unlocked(name, buf_out, max_len);
    kmutex_unlock(&vfs_mutex);
    return r;
}

int vfs_delete(const char *name)
{
    kmutex_lock(&vfs_mutex);
    if (!fs_initialized) vfs_init();
    if (is_proc_path(name) || is_dev_path(name)) { kmutex_unlock(&vfs_mutex); return -1; }
    if (agent_path_denied(name)) { kmutex_unlock(&vfs_mutex); return -1; }

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used && str_eq(files[i].name, name)) {
            files[i].used = 0;
            files[i].size = 0;
            files[i].name[0] = '\0';
            files[i].dirty = 1;
            int r = vfs_sync_unlocked();
            kmutex_unlock(&vfs_mutex);
            return r != 0 ? -1 : 0;
        }
    }
    kmutex_unlock(&vfs_mutex);
    return -1;
}

void vfs_list(void)
{
    kmutex_lock(&vfs_mutex);
    if (!fs_initialized) vfs_init();

    kprint("\nARCHIVOS EN RamFS (Persistente en virtio-blk):\n");
    kprint("--------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            kprint("  "); kprint(files[i].name);
            uint32_t nl = str_len(files[i].name);
            for (uint32_t s = nl; s < 30; ++s) kputc(' ');
            kprint_dec(files[i].size); kprint(" bytes\n");
            count++;
        }
    }
    kprint("\nPSEUDO-FILESYSTEM (Memoria Virtual):\n");
    kprint("  /proc/version, /proc/uptime, /proc/meminfo\n");
    kprint("  /dev/null, /dev/zero, /dev/urandom, /dev/rtc\n");
    kprint("--------------------------------------------------\n");
    kprint("Total: "); kprint_dec((uint32_t)count); kprint(" archivos persistentes.\n\n");
    kmutex_unlock(&vfs_mutex);
}

int vfs_format_list(char *out_buf, uint32_t max)
{
    kmutex_lock(&vfs_mutex);
    if (!fs_initialized) vfs_init();
    uint32_t pos = 0;
    const char *hdr = "Archivos: ";
    while (*hdr && pos < max - 1) out_buf[pos++] = *hdr++;

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].used) {
            if (count > 0 && pos < max - 2) { out_buf[pos++] = ','; out_buf[pos++] = ' '; }
            const char *n = files[i].name;
            while (*n && pos < max - 1) out_buf[pos++] = *n++;
            count++;
        }
    }
    out_buf[pos] = '\0';
    kmutex_unlock(&vfs_mutex);
    return (int)pos;
}

void vfs_get_stats(uint32_t *files_used, uint32_t *bytes_used, uint32_t *dirty_count)
{
    kmutex_lock(&vfs_mutex);
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
    kmutex_unlock(&vfs_mutex);
}

void vfs_tree(char *out_buf, uint32_t max)
{
    kmutex_lock(&vfs_mutex);
    uint32_t pos = 0;
    const char *hdr = "/ (SOMAFS01 + PseudoFS V2)\n+-- dev/\n|   +-- null\n|   +-- zero\n|   +-- urandom\n|   +-- rtc\n+-- proc/\n|   +-- version\n|   +-- uptime\n|   +-- meminfo\n|   +-- cpuinfo\n|   +-- threads\n|   +-- net/\n|       +-- dev\n|       +-- arp\n";
    kprint(hdr);
    fb_puts(out_buf, max, &pos, hdr);

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].used) continue;
        const char *fn = files[i].name;
        if (fn[0] == '/') fn++;
        kprint("+-- "); kprint(fn); kprint("\n");
        fb_puts(out_buf, max, &pos, "+-- ");
        fb_puts(out_buf, max, &pos, fn);
        fb_puts(out_buf, max, &pos, "\n");
    }
    out_buf[pos] = '\0';
    kmutex_unlock(&vfs_mutex);
}

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
    uint32_t len = str_len(initial_data);
    int r = vfs_create_unlocked(name, initial_data, len);
    kmutex_unlock(&vfs_mutex);
    return r;
}

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

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        load_file_from_disk(i);
    }

    kmutex_unlock(&vfs_mutex);
    return 0;
}
