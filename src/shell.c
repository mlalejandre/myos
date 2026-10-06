#include "httpd.h"
#include "shell.h"
#include "console.h"
#include "fs.h"
#include "mem.h"
#include "thread.h"
#include "mutex.h"
#include "pmm.h"
#include "vmm.h"
#include "sysinfo.h"
#include "pci.h"
#include "net.h"
#include "ip.h"
#include "dns.h"
#include "http.h"
#include "virtio_blk.h"
#include "srcfs.h"
#include "boot_gate.h"
#include "rtc.h"
#include "nano.h"
#include "agent.h"
#include "llm.h"
#include "io.h"
#include "arch.h"
#include "idt.h"
#include "disk_layout.h"

#ifndef SECTOR_USER_MIN
#define SECTOR_USER_MIN 2056
#endif

/* ====================================================================
 * UTILIDADES DE CADENA Y COMPARACIÓN
 * ==================================================================== */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static char to_lower_char(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static const char *find_substr_ci(const char *haystack, const char *needle, int ignore_case)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n) {
            char ch = ignore_case ? to_lower_char(*h) : *h;
            char cn = ignore_case ? to_lower_char(*n) : *n;
            if (ch != cn) break;
            h++; n++;
        }
        if (!*n) return haystack;
    }
    return 0;
}

/* ====================================================================
 * SUBSISTEMA DE VARIABLES DE ENTORNO (ENV BARE-METAL)
 * ==================================================================== */

#define MAX_ENV_VARS 32
#define ENV_KEY_LEN  32
#define ENV_VAL_LEN  96

struct env_var {
    char key[ENV_KEY_LEN];
    char val[ENV_VAL_LEN];
    int  used;
    int  pad;
} __attribute__((aligned(8)));

static struct env_var env_table[MAX_ENV_VARS];
static int env_initialized = 0;

static const char *pipe_stdin_data = 0;
static uint32_t    pipe_stdin_len  = 0;

static void env_init_defaults(void)
{
    if (env_initialized) return;
    for (int i = 0; i < MAX_ENV_VARS; ++i) env_table[i].used = 0;

    int idx = 0;
    #define SET_DEF(k, v) do { \
        uint32_t p=0; while((k)[p] && p < ENV_KEY_LEN-1){ env_table[idx].key[p]=(k)[p]; p++; } env_table[idx].key[p]='\0'; \
        p=0; while((v)[p] && p < ENV_VAL_LEN-1){ env_table[idx].val[p]=(v)[p]; p++; } env_table[idx].val[p]='\0'; \
        env_table[idx].used = 1; idx++; \
    } while(0)

    SET_DEF("USER", "marcos");
    SET_DEF("HOST", "soma-node1");
    SET_DEF("ARCH", MYOS_ARCH_NAME);
    SET_DEF("OS", "SOMA-0.2");
    SET_DEF("SHELL", "/bin/soma");
    SET_DEF("PWD", "/");
    #undef SET_DEF

    env_initialized = 1;
}

static const char *env_get(const char *key)
{
    if (!env_initialized) env_init_defaults();
    if (!key || key[0] == '\0') return "";

    for (int i = 0; i < MAX_ENV_VARS; ++i) {
        if (env_table[i].used && str_eq(env_table[i].key, key)) {
            return env_table[i].val;
        }
    }
    return "";
}

static int env_set(const char *key, const char *val)
{
    if (!env_initialized) env_init_defaults();
    if (!key || key[0] == '\0') return -1;

    for (int i = 0; i < MAX_ENV_VARS; ++i) {
        if (env_table[i].used && str_eq(env_table[i].key, key)) {
            uint32_t p = 0;
            while (val && val[p] && p < ENV_VAL_LEN - 1) { env_table[i].val[p] = val[p]; p++; }
            env_table[i].val[p] = '\0';
            return 0;
        }
    }
    for (int i = 0; i < MAX_ENV_VARS; ++i) {
        if (!env_table[i].used) {
            uint32_t p = 0;
            while (key[p] && p < ENV_KEY_LEN - 1) { env_table[i].key[p] = key[p]; p++; }
            env_table[i].key[p] = '\0';
            p = 0;
            while (val && val[p] && p < ENV_VAL_LEN - 1) { env_table[i].val[p] = val[p]; p++; }
            env_table[i].val[p] = '\0';
            env_table[i].used = 1;
            return 0;
        }
    }
    return -1;
}

static int env_unset(const char *key)
{
    if (!env_initialized) env_init_defaults();
    for (int i = 0; i < MAX_ENV_VARS; ++i) {
        if (env_table[i].used && str_eq(env_table[i].key, key)) {
            env_table[i].used = 0;
            return 0;
        }
    }
    return -1;
}

static void env_expand(const char *src, char *dst, uint32_t max_dst)
{
    if (!env_initialized) env_init_defaults();
    if (!src || !dst || max_dst == 0) return;

    uint32_t dp = 0;
    while (*src && dp < max_dst - 1) {
        if (*src == '$' && *(src + 1) != '\0' && *(src + 1) != ' ' && *(src + 1) != '"' && *(src + 1) != '\'') {
            src++;
            char k_buf[ENV_KEY_LEN];
            uint32_t kp = 0;
            while ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') || (*src >= '0' && *src <= '9') || *src == '_') {
                if (kp < sizeof(k_buf) - 1) k_buf[kp++] = *src;
                src++;
            }
            k_buf[kp] = '\0';
            const char *val = env_get(k_buf);
            while (*val && dp < max_dst - 1) dst[dp++] = *val++;
        } else {
            dst[dp++] = *src++;
        }
    }
    dst[dp] = '\0';
}

/* ====================================================================
 * FORMATEO DE BUFFER Y HARDWARE
 * ==================================================================== */

struct bg_job {
    char cmd[256];
    uint32_t tid;
    char log_file[64];
    uint8_t agent_origin;
};

/* AGENT_POLICY_V1: frontera de privilegios del Agente ReAct. */
/* PARCHE 046: el modo Agente es POR HILO (tcb.agent_mode). Con un flag global,
 * un job del agente (spawn) que dormia heredaba privilegios completos cuando
 * otro hilo restauraba el flag a 0. */
static int agent_mode_fallback = 0;
static int *agent_mode_ptr(void)
{
    struct tcb *t = thread_current();
    return t ? &t->agent_mode : &agent_mode_fallback;
}
#define agent_dispatch_mode (*agent_mode_ptr())

int dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out);

int dispatch_agent_command(const char *cmd_line, char *out_buf, uint32_t max_out)
{
    int previous = agent_dispatch_mode;
    agent_dispatch_mode = 1;

    int result = dispatch_command(cmd_line, out_buf, max_out);

    agent_dispatch_mode = previous;
    return result;
}

static void fb_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    if (!out || max == 0 || !pos || *pos >= max - 1) return;
    while (*s && *pos + 1 < max) {
        out[(*pos)++] = *s++;
    }
    out[*pos] = '\0';
}

static void fb_putc(char *out, uint32_t max, uint32_t *pos, char c)
{
    if (!out || max == 0) return;
    if (*pos + 1 < max) {
        out[(*pos)++] = c;
        out[*pos] = '\0';
    }
}

static void fb_put_dec(char *out, uint32_t max, uint32_t *pos, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { fb_putc(out, max, pos, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) fb_putc(out, max, pos, tmp[--n]);
}

static void fb_put_hex16(char *out, uint32_t max, uint32_t *pos, uint16_t v)
{
    const char hex[] = "0123456789ABCDEF";
    fb_putc(out, max, pos, hex[(v >> 12) & 0xF]);
    fb_putc(out, max, pos, hex[(v >> 8) & 0xF]);
    fb_putc(out, max, pos, hex[(v >> 4) & 0xF]);
    fb_putc(out, max, pos, hex[v & 0xF]);
}

static void fb_put_hex32(char *out, uint32_t max, uint32_t *pos, uint32_t v)
{
    fb_put_hex16(out, max, pos, (uint16_t)(v >> 16));
    fb_put_hex16(out, max, pos, (uint16_t)v);
}

static void parse_ip(const char *s, uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
        ip[i] = (uint8_t)val;
        if (*s == '.') s++;
    }
}

static uint64_t parse_num(const char **str)
{
    uint64_t val = 0;
    while (**str == ' ') (*str)++;
    while (**str >= '0' && **str <= '9') { val = val * 10 + (**str - '0'); (*str)++; }
    return val;
}

static void system_reboot(void)
{
    kprint("\n[REBOOT] Sincronizando discos...\n");
    vfs_sync();
#ifdef __x86_64__
    for (int i = 0; i < 10000; ++i) if ((inb(0x64) & 0x02) == 0) break;
    outb(0x64, 0xFE);
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idtr = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idtr));
#endif
    for (;;) { arch_interrupts_disable(); arch_halt(); }
}

static void system_poweroff(void)
{
    kprint("\n[POWEROFF] Sincronizando discos...\n");
    vfs_sync();
#ifdef __x86_64__
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outb(0xF4, 0x00);
#endif
    for (;;) { arch_interrupts_disable(); arch_halt(); }
}

static const char *pci_device_name(uint16_t vendor, uint16_t device)
{
    if (vendor == 0x8086 && device == 0x1237) return "Intel 440FX Host Bridge";
    if (vendor == 0x8086 && device == 0x7000) return "Intel PIIX3 ISA Bridge";
    if (vendor == 0x8086 && device == 0x7010) return "Intel PIIX3 IDE";
    if (vendor == 0x8086 && device == 0x7113) return "Intel PIIX4 ACPI";
    if (vendor == 0x1234 && device == 0x1111) return "QEMU Standard VGA";
    if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) return "VirtIO-NET Adapter";
    if (vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042)) return "VirtIO-BLK Storage";
    return "Dispositivo PCI generico";
}

void pci_scan(void)
{
    kprint("\nPCI SCAN\n--------\n");
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint16_t vendor = pci_vendor_id((uint8_t)bus, slot, func);
                if (vendor == 0xFFFF) continue;
                uint16_t dev = pci_device_id((uint8_t)bus, slot, func);
                kprint("PCI "); kprint_hex16(bus); kputc(':'); kprint_dec(slot); kputc('.'); kprint_dec(func);
                kprint(" vendor=0x"); kprint_hex16(vendor); kprint(" device=0x"); kprint_hex16(dev);
                if (vendor == 0x1AF4 && (dev == 0x1000 || dev == 0x1041)) kprint("  <-- VIRTIO-NET");
                if (vendor == 0x1AF4 && (dev == 0x1001 || dev == 0x1042)) kprint("  <-- VIRTIO-BLK");
                kprint("\n");
            }
        }
    }
}

static int pci_format_scan(char *out, uint32_t max)
{
    uint32_t pos = 0;
    fb_puts(out, max, &pos, "Dispositivos PCI detectados:\n");
    uint32_t count = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint16_t vendor = pci_vendor_id((uint8_t)bus, slot, func);
                if (vendor == 0xFFFF) continue;
                uint16_t dev = pci_device_id((uint8_t)bus, slot, func);
                count++;

                fb_puts(out, max, &pos, "  PCI ");
                fb_put_hex16(out, max, &pos, bus);
                fb_putc(out, max, &pos, ':');
                fb_put_dec(out, max, &pos, slot);
                fb_putc(out, max, &pos, '.');
                fb_put_dec(out, max, &pos, func);
                fb_puts(out, max, &pos, " [0x");
                fb_put_hex16(out, max, &pos, vendor);
                fb_puts(out, max, &pos, ":0x");
                fb_put_hex16(out, max, &pos, dev);
                fb_puts(out, max, &pos, "] ");
                fb_puts(out, max, &pos, pci_device_name(vendor, dev));

                if (vendor == 0x1AF4) {
                    uint32_t bar0 = pci_bar0((uint8_t)bus, slot, func);
                    fb_puts(out, max, &pos, " (BAR0=0x");
                    fb_put_hex32(out, max, &pos, bar0 & 0xFFFFFFFC);
                    fb_putc(out, max, &pos, ')');
                }
                fb_putc(out, max, &pos, '\n');
            }
        }
    }
    return (int)pos;
}

static void do_hexdump(const char *filename, uint32_t max_bytes, char *out_buf, uint32_t max_out)
{
    static char hbuf[4096];
    uint32_t pos = 0;
    int r = vfs_read(filename, hbuf, sizeof(hbuf));
    if (r < 0) {
        kprint("hexdump: archivo no encontrado: '"); kprint(filename); kprint("'\n");
        if (out_buf) fb_puts(out_buf, max_out, &pos, "hexdump: archivo no encontrado");
        return;
    }
    uint32_t total = (uint32_t)r;
    uint32_t limit = (max_bytes > 0 && max_bytes < total) ? max_bytes : total;
    if (limit > 128) limit = 128;

    kprint("\nHEXDUMP de '"); kprint(filename); kprint("' ("); kprint_dec(total); kprint(" bytes):\n");
    const char hexchars[] = "0123456789ABCDEF";
    for (uint32_t off = 0; off < limit; off += 16) {
        for (int sh = 28; sh >= 0; sh -= 4) kputc(hexchars[(off >> sh) & 0x0F]);
        kprint(": ");
        for (uint32_t i = 0; i < 16; ++i) {
            if (off + i < limit) {
                uint8_t b = (uint8_t)hbuf[off + i];
                kputc(hexchars[(b >> 4) & 0x0F]);
                kputc(hexchars[b & 0x0F]);
                kputc(' ');
            } else {
                kprint("   ");
            }
            if (i == 7) kputc(' ');
        }
        kprint(" |");
        for (uint32_t i = 0; i < 16 && off + i < limit; ++i) {
            uint8_t c = (uint8_t)hbuf[off + i];
            kputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        kprint("|\n");
    }
    if (out_buf) fb_puts(out_buf, max_out, &pos, "[HEXDUMP OK]");
}

void show_somafetch(char *out_buf, uint32_t max_out)
{
    uint32_t pos = 0;
    uint64_t ms = timer_get_uptime_ms();
    uint32_t s = (uint32_t)(ms / 1000);
    uint32_t m = s / 60; s %= 60;
    uint32_t h = m / 60; m %= 60;

    size_t f_free = 0, f_used = 0, f_total = 0;
    pmm_get_stats(&f_free, &f_used, &f_total);
    uint32_t ram_tot = (uint32_t)((f_total * 4096) / (1024 * 1024));
    uint32_t ram_usd = (uint32_t)((f_used * 4096) / (1024 * 1024));
    uint32_t ram_pct = f_total ? (uint32_t)((f_used * 100) / f_total) : 0;

    size_t h_used = 0, h_free = 0;
    kheap_stats(&h_used, &h_free);

    kprint("\n");
    kprint("\033[1;36m  ____  ____  __  __    _      \033[1;33mOS:     \033[1;37mSOMA v0.2 \033[0;36m(Multi-Agente)\033[0m\n");
    kprint("\033[1;36m / ___|/ __ \\|  \\/  |  / \\     \033[1;33mKernel: \033[1;37m" MYOS_ARCH_NAME " Bare-Metal \033[1;32m(HAL v1)\033[0m\n");
    kprint("\033[1;36m \\___ \\ |  | | |\\/| | / _ \\    \033[1;33mUptime: \033[1;37m");
    if (h > 0) { kprint_dec(h); kprint("h "); }
    if (m > 0 || h > 0) { kprint_dec(m); kprint("m "); }
    kprint_dec(s); kprint("s \033[0;32m(PIT 1 kHz)\033[0m\n");
    kprint("\033[1;36m  ___)| |__| | |  | |/ ___ \\   \033[1;33mCPU:    \033[1;37mRing 0 \033[1;35m(CR0:WP, NXE)\033[0m\n");
    kprint("\033[1;36m |____/\\____/|_|  |_/_/   \\_\\  \033[1;33mRAM:    \033[1;37m");
    kprint_dec(ram_usd); kprint("/"); kprint_dec(ram_tot); kprint(" MiB \033[1;32m("); kprint_dec(ram_pct); kprint("% PMM)\033[0m\n");
    kprint("                               \033[1;33mHeap:   \033[1;37m");
    kprint_dec((uint32_t)(h_free / 1024)); kprint(" KiB libres\033[0m\n");
    kprint("                               \033[1;33mDisco:  \033[1;37mRamFS (16 MiB virtio-blk)\033[0m\n");
    kprint("                               \033[1;33mRed:    \033[1;37m10.0.2.15 (VirtIO-NET NAT)\033[0m\n");
    kprint("                               \033[1;33mAgente: \033[1;37mnail-35b (ReAct LAN)\033[0m\n\n");

    fb_puts(out_buf, max_out, &pos, "SOMA v0.2 Operational");
}

static void spawn_job_worker(void *arg)
{
    struct bg_job *job = (struct bg_job *)arg;
    if (!job) return;

    char *out_buf = (char *)kmalloc(2048);
    if (out_buf) {
        out_buf[0] = '\0';
        if (job->agent_origin) {
            dispatch_agent_command(job->cmd, out_buf, 2048);
        } else {
            dispatch_command(job->cmd, out_buf, 2048);
        }
        if (out_buf[0] != '\0') {
            uint32_t len = 0; while (out_buf[len]) len++;
            vfs_write(job->log_file, out_buf, len);
        }
        kfree(out_buf);
    }

    kprint("\n[JOB TID "); kprint_dec(job->tid); kprint(" FINALIZADO -> "); kprint(job->log_file); kprint("]\nsoma> ");
    kfree(job);
}

/* ====================================================================
 * MANEJADORES DE COMANDO DECLARATIVOS (cmd_handler_t)
 * ==================================================================== */

static int cmd_help(const char *args, char *out_buf, uint32_t max_out);

static int cmd_somafetch(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    show_somafetch(out_buf, max_out);
    return 1;
}

static int cmd_creador(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    kprint("SOMA (Sistema Operativo Multi-Agente) - Creado por Marcos\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "SOMA - Creado por Marcos");
    return 1;
}

static int cmd_status(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    kprint("ARCH: "); kprint(MYOS_ARCH_NAME);
    kprint(" (HAL v1) | IP: "); kprint_ip(net_ip);
    kprint(" | GW: "); kprint_ip(net_gateway);
    kprint(" | MAC: "); kprint_mac(net_mac);
    kprint("\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "STATUS OK");
    return 1;
}

static int cmd_uptime(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    uint64_t ms = timer_get_uptime_ms();
    kprint("Uptime: "); kprint_dec((uint32_t)(ms / 1000)); kprint("s\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "uptime ejecutado");
    return 1;
}

static int cmd_date(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    struct rtc_time t; rtc_get_datetime(&t);
    char date_str[64]; uint32_t dp = 0;
    fb_puts(date_str, sizeof(date_str), &dp, "RTC: ");
    fb_put_dec(date_str, sizeof(date_str), &dp, t.year); fb_putc(date_str, sizeof(date_str), &dp, '-');
    if (t.month < 10) fb_putc(date_str, sizeof(date_str), &dp, '0');
    fb_put_dec(date_str, sizeof(date_str), &dp, t.month); fb_putc(date_str, sizeof(date_str), &dp, '-');
    if (t.day < 10) fb_putc(date_str, sizeof(date_str), &dp, '0');
    fb_put_dec(date_str, sizeof(date_str), &dp, t.day); fb_putc(date_str, sizeof(date_str), &dp, ' ');
    if (t.hour < 10) fb_putc(date_str, sizeof(date_str), &dp, '0');
    fb_put_dec(date_str, sizeof(date_str), &dp, t.hour); fb_putc(date_str, sizeof(date_str), &dp, ':');
    if (t.min < 10) fb_putc(date_str, sizeof(date_str), &dp, '0');
    fb_put_dec(date_str, sizeof(date_str), &dp, t.min); fb_putc(date_str, sizeof(date_str), &dp, ':');
    if (t.sec < 10) fb_putc(date_str, sizeof(date_str), &dp, '0');
    fb_put_dec(date_str, sizeof(date_str), &dp, t.sec); fb_puts(date_str, sizeof(date_str), &dp, " UTC");
    kprint(date_str); kprint("\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, date_str);
    return 1;
}

static int cmd_clear(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    console_clear();
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Pantalla borrada.");
    return 1;
}

static int cmd_test_suite(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    boot_gate_run_test_suite(out_buf, max_out);
    return 1;
}

static int cmd_mem(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    sysinfo_print_mem();
    sysinfo_format_mem(out_buf, max_out);
    return 1;
}

static int cmd_free(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    size_t f_free = 0, f_used = 0, f_total = 0; pmm_get_stats(&f_free, &f_used, &f_total);
    size_t h_used = 0, h_free = 0; kheap_stats(&h_used, &h_free);
    kprint("\nRAM: "); kprint_dec((uint32_t)((f_used * 4096)/(1024*1024))); kprint("/"); kprint_dec((uint32_t)((f_total * 4096)/(1024*1024))); kprint(" MiB usados\n");
    kprint("Heap: "); kprint_dec((uint32_t)(h_free / 1024)); kprint(" KiB libres\n\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "free ejecutado");
    return 1;
}

static int cmd_df(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    uint32_t files_u = 0, bytes_u = 0, dirty_c = 0;
    vfs_get_stats(&files_u, &bytes_u, &dirty_c);
    kprint("\nALMACENAMIENTO (df):\nRamFS: "); kprint_dec(files_u); kprint("/32 archivos | ");
    kprint_dec(bytes_u / 1024); kprint(" KiB usados\nInodos dirty pendientes: "); kprint_dec(dirty_c); kprint("\n\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "df ejecutado");
    return 1;
}

static int cmd_stats(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    sysinfo_print_stats();
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "stats ejecutado");
    return 1;
}

static int cmd_heap(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    kheap_dump_stats();
    kheap_test_self();
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Heap inspeccionado");
    return 1;
}

static int cmd_pmm(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    pmm_dump_stats();
    pmm_test_self();
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "PMM: Verificado.");
    return 1;
}

static int cmd_vmm(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    vmm_test_self();
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "VMM: Verificado.");
    return 1;
}

static int cmd_pci(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    pci_scan();
    pci_format_scan(out_buf, max_out);
    return 1;
}

static int cmd_arp(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    arp_format_cache(out_buf, max_out);
    kprint(out_buf);
    return 1;
}

static int cmd_ps(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    thread_dump();
    thread_format_table(out_buf, max_out);
    return 1;
}

static int cmd_threads(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    thread_dump();
    thread_format_table(out_buf, max_out);
    return 1;
}

static int cmd_spawn(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: spawn <comando>  (alias: bg <comando>)\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Uso: spawn <comando>");
        return 1;
    }

    struct bg_job *job = (struct bg_job *)kmalloc(sizeof(struct bg_job));
    if (!job) {
        kprint("Error: Sin memoria para spawn.\n");
        return 1;
    }
    memset(job, 0, sizeof(*job));

    /* AGENT_POLICY_V1: capturar el origen antes de lanzar el hilo. */
    job->agent_origin = (uint8_t)(agent_dispatch_mode != 0);

    uint32_t ci = 0;
    while (args[ci] && ci < sizeof(job->cmd) - 1) {
        job->cmd[ci] = args[ci];
        ci++;
    }
    job->cmd[ci] = '\0';

    char th_name[32];
    th_name[0] = 'b'; th_name[1] = 'g'; th_name[2] = '_';
    uint32_t ni = 3;
    for (uint32_t i = 0; args[i] && args[i] != ' ' && ni < sizeof(th_name) - 1; ++i) {
        th_name[ni++] = args[i];
    }
    th_name[ni] = '\0';

    struct tcb *nt = thread_create(th_name, spawn_job_worker, job);
    if (!nt) {
        kprint("Error: Fallo al crear hilo para spawn.\n");
        kfree(job);
        return 1;
    }

    job->tid = nt->tid;

    /* Formatear /tmp/job_<TID>.log limpiamente */
    uint32_t li = 0;
    const char *pfx = "/tmp/job_";
    while (*pfx) job->log_file[li++] = *pfx++;
    char tbuf[10]; int tn = 0; uint32_t tid_tmp = job->tid;
    if (tid_tmp == 0) tbuf[tn++] = '0';
    else { while (tid_tmp) { tbuf[tn++] = (char)('0' + (tid_tmp % 10)); tid_tmp /= 10; } }
    while (tn > 0) job->log_file[li++] = tbuf[--tn];
    const char *sfx = ".log";
    while (*sfx) job->log_file[li++] = *sfx++;
    job->log_file[li] = '\0';

    kprint("[SPAWN] Tarea '"); kprint(job->cmd);
    kprint("' lanzada en segundo plano [TID "); kprint_dec(job->tid);
    kprint(", Hilo '"); kprint(th_name);
    kprint("'] -> Log: "); kprint(job->log_file); kprint("\n");

    uint32_t pos = 0;
    fb_puts(out_buf, max_out, &pos, "[SPAWN OK] TID=");
    fb_put_dec(out_buf, max_out, &pos, job->tid);
    fb_puts(out_buf, max_out, &pos, " Log=");
    fb_puts(out_buf, max_out, &pos, job->log_file);
    return 1;
}

static int cmd_kill(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    uint32_t tid = 0; while (*args >= '0' && *args <= '9') tid = tid * 10 + (*args++ - '0');
    thread_kill(tid);
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Kill ejecutado");
    return 1;
}

static int cmd_sleep(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    uint32_t ms = 0; while (*args >= '0' && *args <= '9') ms = ms * 10 + (*args++ - '0');
    if (ms == 0) ms = 1000;
    thread_sleep(ms);
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "sleep completado");
    return 1;
}

static int cmd_time(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: time <comando>\n");
        return 1;
    }
    uint64_t t0 = timer_get_uptime_ms(); uint64_t c0 = rdtsc();
    dispatch_command(args, out_buf, max_out);
    uint64_t c1 = rdtsc(); uint64_t t1 = timer_get_uptime_ms();
    kprint("\n[TIME]: "); kprint_dec((uint32_t)(t1 - t0)); kprint(" ms | "); kprint_dec((uint32_t)(c1 - c0)); kprint(" ciclos TSC\n");
    return 1;
}

static int cmd_reboot(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args; (void)out_buf; (void)max_out;
    system_reboot();
    return 1;
}

static int cmd_poweroff(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args; (void)out_buf; (void)max_out;
    system_poweroff();
    return 1;
}

static int cmd_halt(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args; (void)out_buf; (void)max_out;
    vfs_sync();
    kprint("Sistema detenido (HALT).\n");
    for (;;) { arch_interrupts_disable(); arch_halt(); }
    return 1;
}

static int cmd_ls(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    vfs_list();
    vfs_format_list(out_buf, max_out);
    return 1;
}

static int cmd_cat(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        if (pipe_stdin_data != 0) {
            kprint(pipe_stdin_data);
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, pipe_stdin_data);
            return 1;
        }
        kprint("Uso: cat <archivo>\n");
        return 1;
    }
    char buf[2048];
    int r = vfs_read(args, buf, sizeof(buf));
    if (r >= 0) {
        kprint("\n"); kprint(buf); kprint("\n");
        uint32_t i = 0; while (buf[i] && i < max_out - 1) { out_buf[i] = buf[i]; i++; } out_buf[i] = '\0';
    } else {
        kprint("Error: archivo no encontrado: '"); kprint(args); kprint("'\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Error: archivo no encontrado.");
    }
    return 1;
}

static int cmd_write(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char fn[48]; uint32_t fi = 0;
    while (*args && *args != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *args++;
    fn[fi] = '\0';
    while (*args == ' ') args++;

    if (agent_dispatch_mode && (str_eq(fn, "/etc/init.sh") || str_eq(fn, "/etc/soma_manifest.txt"))) {
        kprint("write: operacion denegada por seguridad (Agente no puede modificar scripts de arranque).\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Error: Operacion denegada por politica de seguridad.");
        return 1;
    }

    char q = 0;
    if (*args == '"' || *args == '\'') {
        q = *args++;
    }

    char payload[2048];
    uint32_t pi = 0;
    while (*args && pi < sizeof(payload) - 1) {
        if (q && *args == q && *(args + 1) == '\0') {
            break;
        }
        if (*args == '\\' && *(args + 1) == 'n') {
            payload[pi++] = '\n';
            args += 2;
            continue;
        }
        if (*args == '\\' && *(args + 1) == 't') {
            payload[pi++] = '\t';
            args += 2;
            continue;
        }
        payload[pi++] = *args++;
    }
    payload[pi] = '\0';

    int wres = vfs_write(fn, payload, pi);
    if (wres == -2) {
        kprint("write: error, archivo demasiado grande (max 4095 bytes).\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Error: El archivo excede la cuota maxima de 4095 bytes.");
        return 1;
    }
    if (wres < 0) {
        kprint("write: error al escribir '"); kprint(fn); kprint("' (acceso denegado o fallo de disco).\n");
        uint32_t epos = 0;
        fb_puts(out_buf, max_out, &epos, "Error: no se pudo escribir el archivo (denegado o fallo de I/O).");
        return 1;
    }
    kprint("Escrito en '"); kprint(fn); kprint("' (");
    kprint_dec(pi); kprint(" bytes)\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Archivo escrito.");
    return 1;
}

static int cmd_rm(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    vfs_delete(args);
    kprint("Eliminado '"); kprint(args); kprint("'\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Archivo eliminado.");
    return 1;
}

static int cmd_cp(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char src[48], dst[48]; uint32_t si = 0, di = 0;
    while (*args && *args != ' ' && si < sizeof(src) - 1) src[si++] = *args++;
    src[si] = '\0';
    while (*args == ' ') args++;
    while (*args && *args != ' ' && di < sizeof(dst) - 1) dst[di++] = *args++;
    dst[di] = '\0';
    static char cp_buf[4096]; int r = vfs_read(src, cp_buf, sizeof(cp_buf));
    uint32_t pos = 0;
    if (r < 0) { kprint("cp: origen no encontrado\n"); fb_puts(out_buf, max_out, &pos, "cp: error"); }
    else if (vfs_write(dst, cp_buf, (uint32_t)r) >= 0) { kprint("cp: copiado OK\n"); fb_puts(out_buf, max_out, &pos, "cp: OK"); }
    return 1;
}

static int cmd_mv(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char src[48], dst[48]; uint32_t si = 0, di = 0;
    while (*args && *args != ' ' && si < sizeof(src) - 1) src[si++] = *args++;
    src[si] = '\0';
    while (*args == ' ') args++;
    while (*args && *args != ' ' && di < sizeof(dst) - 1) dst[di++] = *args++;
    dst[di] = '\0';
    static char mv_buf[4096]; int r = vfs_read(src, mv_buf, sizeof(mv_buf));
    uint32_t pos = 0;
    if (r < 0) { kprint("mv: origen no encontrado\n"); fb_puts(out_buf, max_out, &pos, "mv: error"); }
    else if (vfs_write(dst, mv_buf, (uint32_t)r) >= 0) { vfs_delete(src); kprint("mv: movido OK\n"); fb_puts(out_buf, max_out, &pos, "mv: OK"); }
    return 1;
}

static int cmd_touch(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char tmp[8]; uint32_t pos = 0;
    if (vfs_read(args, tmp, sizeof(tmp)) < 0) { vfs_write(args, "", 0); kprint("touch: creado\n"); fb_puts(out_buf, max_out, &pos, "touch: OK"); }
    else { kprint("touch: ya existe\n"); fb_puts(out_buf, max_out, &pos, "touch: existe"); }
    return 1;
}

static int cmd_tree(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    vfs_tree(out_buf, max_out);
    return 1;
}

static int cmd_head(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char fn[48]; uint32_t fi = 0;
    uint32_t lines = 10;
    const char *src_text = 0;
    static char hbuf[4096];

    if (pipe_stdin_data != 0 && (*args == '\0' || (*args >= '0' && *args <= '9'))) {
        src_text = pipe_stdin_data;
        if (*args >= '0' && *args <= '9') {
            lines = 0; while (*args >= '0' && *args <= '9') lines = lines * 10 + (*args++ - '0');
        }
    } else {
        while (*args && *args != ' ' && fi < sizeof(fn) - 1) {
            fn[fi++] = *args++;
        }
        fn[fi] = '\0';
        while (*args == ' ') args++;
        if (*args >= '0' && *args <= '9') {
            lines = 0; while (*args >= '0' && *args <= '9') lines = lines * 10 + (*args++ - '0');
        }
        int r = vfs_read(fn, hbuf, sizeof(hbuf));
        if (r < 0) { uint32_t p = 0; fb_puts(out_buf, max_out, &p, "head: no encontrado"); return 1; }
        hbuf[r] = '\0';
        src_text = hbuf;
    }

    uint32_t lc = 0, pos = 0; kprint("\n");
    for (int i = 0; src_text[i] && lc < lines; ++i) {
        kputc(src_text[i]);
        if (pos < max_out - 1) out_buf[pos++] = src_text[i];
        if (src_text[i] == '\n') lc++;
    }
    out_buf[pos] = '\0'; kprint("\n");
    return 1;
}

static int cmd_tail(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char fn[48]; uint32_t fi = 0;
    uint32_t lines = 10;
    const char *src_text = 0;
    static char tbuf[4096];
    uint32_t total_len = 0;

    if (pipe_stdin_data != 0 && (*args == '\0' || (*args >= '0' && *args <= '9'))) {
        src_text = pipe_stdin_data;
        total_len = pipe_stdin_len;
        if (*args >= '0' && *args <= '9') {
            lines = 0; while (*args >= '0' && *args <= '9') lines = lines * 10 + (*args++ - '0');
        }
    } else {
        while (*args && *args != ' ' && fi < sizeof(fn) - 1) {
            fn[fi++] = *args++;
        }
        fn[fi] = '\0';
        while (*args == ' ') args++;
        if (*args >= '0' && *args <= '9') {
            lines = 0; while (*args >= '0' && *args <= '9') lines = lines * 10 + (*args++ - '0');
        }
        int r = vfs_read(fn, tbuf, sizeof(tbuf));
        if (r < 0) { uint32_t p = 0; fb_puts(out_buf, max_out, &p, "tail: no encontrado"); return 1; }
        total_len = (uint32_t)r;
        tbuf[r] = '\0';
        src_text = tbuf;
    }

    uint32_t tl = 0; for (uint32_t i = 0; i < total_len; ++i) if (src_text[i] == '\n') tl++;
    uint32_t skip = (tl > lines) ? (tl - lines) : 0, cl = 0, si = 0;
    while (si < total_len && cl < skip) if (src_text[si++] == '\n') cl++;
    uint32_t pos = 0; kprint("\n");
    for (uint32_t i = si; i < total_len; ++i) {
        kputc(src_text[i]);
        if (pos < max_out - 1) out_buf[pos++] = src_text[i];
    }
    out_buf[pos] = '\0'; kprint("\n");
    return 1;
}

static int cmd_wc(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    static char wcbuf[4096];
    const char *src_text = 0;
    uint32_t text_len = 0;

    if (*args != '\0') {
        int r = vfs_read(args, wcbuf, sizeof(wcbuf));
        if (r < 0) {
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "wc: no encontrado");
            return 1;
        }
        text_len = (uint32_t)r;
        wcbuf[r] = '\0';
        src_text = wcbuf;
    } else if (pipe_stdin_data != 0) {
        src_text = pipe_stdin_data;
        text_len = pipe_stdin_len;
    } else {
        kprint("Uso: wc [archivo]  (o bien: <cmd> | wc)\n");
        return 1;
    }

    uint32_t l = 0, w = 0; int in_w = 0;
    for (uint32_t i = 0; i < text_len; ++i) {
        if (src_text[i] == '\n') l++;
        if (src_text[i] == ' ' || src_text[i] == '\t' || src_text[i] == '\n' || src_text[i] == '\r') in_w = 0;
        else if (!in_w) { in_w = 1; w++; }
    }
    kprint("  "); kprint_dec(l); kprint(" lineas  "); kprint_dec(w); kprint(" palabras  "); kprint_dec(text_len); kprint(" bytes\n");
    uint32_t pos = 0;
    fb_put_dec(out_buf, max_out, &pos, l); fb_puts(out_buf, max_out, &pos, " lineas, ");
    fb_put_dec(out_buf, max_out, &pos, w); fb_puts(out_buf, max_out, &pos, " palabras, ");
    fb_put_dec(out_buf, max_out, &pos, text_len); fb_puts(out_buf, max_out, &pos, " bytes");
    return 1;
}

static int cmd_grep(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    int ignore_case = 0;
    if (args[0] == '-' && args[1] == 'i' && (args[2] == ' ' || args[2] == '\0')) {
        ignore_case = 1;
        args += 2;
        while (*args == ' ') args++;
    }

    char pat[64]; uint32_t pi = 0; char q = 0;
    if (*args == '\'' || *args == '"') q = *args++;
    while (*args && pi < sizeof(pat) - 1) {
        if (q && *args == q) { args++; break; }
        if (!q && *args == ' ') break;
        pat[pi++] = *args++;
    }
    pat[pi] = '\0';
    while (*args == ' ') args++;

    const char *src_text = 0;
    static char gbuf[4096];

    if (*args != '\0') {
        int r = vfs_read(args, gbuf, sizeof(gbuf) - 1);
        if (r < 0) {
            kprint("grep: archivo no encontrado: '"); kprint(args); kprint("'\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "grep: archivo no encontrado");
            return 1;
        }
        gbuf[r] = '\0';
        src_text = gbuf;
    } else if (pipe_stdin_data != 0) {
        src_text = pipe_stdin_data;
    } else {
        kprint("Uso: grep [-i] <patron> [archivo]  (o bien: <cmd> | grep [-i] <patron>)\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Uso: grep [-i] <patron>");
        return 1;
    }

    uint32_t line_num = 1;
    uint32_t pos = 0;
    int matches = 0;
    const char *p = src_text;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        uint32_t line_len = (uint32_t)(p - line_start);
        char line_tmp[256];
        uint32_t copy_len = line_len < sizeof(line_tmp) - 1 ? line_len : sizeof(line_tmp) - 1;
        for (uint32_t k = 0; k < copy_len; ++k) line_tmp[k] = line_start[k];
        line_tmp[copy_len] = '\0';

        if (find_substr_ci(line_tmp, pat, ignore_case)) {
            matches++;
            kprint_dec(line_num); kprint(": "); kprint(line_tmp); kprint("\n");
            fb_put_dec(out_buf, max_out, &pos, line_num);
            fb_puts(out_buf, max_out, &pos, ": ");
            fb_puts(out_buf, max_out, &pos, line_tmp);
            fb_puts(out_buf, max_out, &pos, "\n");
        }

        line_num++;
        if (*p == '\n') p++;
    }

    if (matches == 0) {
        kprint("grep: sin coincidencias.\n");
        fb_puts(out_buf, max_out, &pos, "grep: sin coincidencias.");
    }
    return 1;
}

static int cmd_echo(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char clean[512];
    uint32_t ci = 0;

    char q = 0;
    if (*args == '"' || *args == '\'') {
        q = *args++;
    }

    while (*args && ci < sizeof(clean) - 1) {
        if (q && *args == q) {
            args++;
            while (*args == ' ') args++;
            if (*args == '"' || *args == '\'') q = *args++;
            else q = 0;
            continue;
        }
        clean[ci++] = *args++;
    }
    clean[ci] = '\0';

    kprint(clean); kprint("\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, clean); fb_putc(out_buf, max_out, &pos, '\n');
    return 1;
}

static int cmd_nano(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    run_nano(args);
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "nano cerrado.");
    return 1;
}

static int cmd_hexdump(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    do_hexdump(args, 128, out_buf, max_out);
    return 1;
}

static int cmd_checkpoint(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    if (vfs_checkpoint_save() == 0) {
        kprint("[CHECKPOINT OK]\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Checkpoint OK");
    }
    return 1;
}

static int cmd_rollback(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    if (vfs_checkpoint_restore() == 0) {
        kprint("[ROLLBACK OK]\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Rollback OK");
    }
    return 1;
}

static int cmd_fs_sync(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    vfs_sync();
    kprint("RamFS sincronizado.\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Sync OK");
    return 1;
}

static int cmd_fs_format(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    vfs_format();
    kprint("RamFS formateado a fabrica.\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Format OK");
    return 1;
}

static int cmd_sector_read(const char *args, char *out_buf, uint32_t max_out)
{
    uint64_t sec = parse_num(&args);
    char sbuf[512];
    if (virtio_blk_read(sec, sbuf) == 0) {
        kprint("\n[LBA "); kprint_dec((uint32_t)sec); kprint("]: ");
        for (int i=0; i<512 && sbuf[i]; i++) if (sbuf[i]>=32 && sbuf[i]<127) kputc(sbuf[i]);
        kprint("\n");
    }
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "sector leido");
    return 1;
}

static int cmd_sector_write(const char *args, char *out_buf, uint32_t max_out)
{
    uint64_t sec = parse_num(&args);
    if (sec < SECTOR_USER_MIN || (sec >= 4095 && sec <= 4400)) {
        kprint("Error: sector protegido.\n");
        return 1;
    }
    while (*args == ' ') args++;
    char sbuf[512]; memset(sbuf, 0, 512); int bi = 0;
    while (*args && bi < 511) sbuf[bi++] = *args++;
    virtio_blk_write(sec, sbuf);
    kprint("Sector escrito.\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "sector escrito");
    return 1;
}

static int cmd_ping(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    uint8_t target[4];
    if (*args) parse_ip(args, target);
    else { target[0] = net_gateway[0]; target[1] = net_gateway[1]; target[2] = net_gateway[2]; target[3] = net_gateway[3]; }
    int r = icmp_ping(target, 1, 2000);
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, (r == 1) ? "Ping exitoso" : "Ping fallido");
    return 1;
}

static int cmd_dns(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    uint8_t ip[4];
    dns_resolve(args, ip, 3000);
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "DNS consultado");
    return 1;
}

static int cmd_curl(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char target[64]; uint32_t ti = 0; while (*args && *args != ' ' && ti < sizeof(target) - 1) target[ti++] = *args++; target[ti] = '\0';
    while (*args == ' ') args++;
    uint16_t port = 80; if (*args >= '0' && *args <= '9') { port = 0; while (*args >= '0' && *args <= '9') port = port * 10 + (*args++ - '0'); }
    while (*args == ' ') args++;
    const char *path = (*args) ? args : "/";
    uint8_t tip[4]; const char *hhdr = 0; int is_ip = 1, dots = 0;
    for (int i = 0; target[i]; ++i) { if (target[i] == '.') dots++; else if (target[i] < '0' || target[i] > '9') is_ip = 0; }
    if (dots != 3) is_ip = 0;
    if (is_ip) parse_ip(target, tip);
    else { if (!dns_resolve(target, tip, 3000)) { kprint("DNS error\n"); return 1; } hhdr = target; }
    static char http_buf[4096]; struct http_response resp;
    int code = http_get_host(tip, port, hhdr, path, http_buf, sizeof(http_buf), &resp, 40000);
    if (code >= 0) {
        const char *disp = (resp.body && resp.body[0]) ? resp.body : http_buf;
        kprint("\n--- RESPUESTA HTTP ["); kprint_dec((uint32_t)code); kprint("] ---\n"); kprint(disp); kprint("\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, disp);
    }
    return 1;
}

static int cmd_soma(const char *args, char *out_buf, uint32_t max_out)
{
    (void)out_buf; (void)max_out;
    agent_run(args);
    return 1;
}

static int cmd_health(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    int ok = llm_health();
    kprint(ok ? "Servidor LLM: OK (HTTP 200 OK)\n" : "Servidor LLM: ERROR\n");
    uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, ok ? "Servidor LLM: OK" : "Servidor LLM: ERROR");
    return 1;
}

static int cmd_llm(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: llm <mensaje>\n");
        return 1;
    }
    static char reply[4096];
    kprint("Consultando a nail-35b...\n");
    if (llm_chat(args, reply, sizeof(reply), 30000)) {
        kprint("\n[nail-35b]: "); kprint(reply); kprint("\n\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, reply);
    } else {
        kprint("Error al consultar el LLM.\n");
    }
    return 1;
}

static int cmd_llm_diag(const char *args, char *out_buf, uint32_t max_out)
{
    static char reply[4096];
    kprint("Solicitando diagnostico al LLM...\n");
    if (llm_diagnose(args, reply, sizeof(reply), 35000)) {
        kprint("\n[DIAGNOSTICO IA]:\n"); kprint(reply); kprint("\n\n");
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, reply);
    } else {
        kprint("Error al solicitar diagnostico al LLM.\n");
    }
    return 1;
}

static int cmd_src_ls(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    srcfs_ls(out_buf, max_out);
    kprint(out_buf); kputc('\n');
    return 1;
}

static int cmd_src_cat(const char *args, char *out_buf, uint32_t max_out)
{
    char full[128]; uint32_t p = 0;
    fb_puts(full, sizeof(full), &p, "src_cat ");
    fb_puts(full, sizeof(full), &p, args);
    src_tool(full, out_buf, max_out);
    return 1;
}

static int cmd_src_grep(const char *args, char *out_buf, uint32_t max_out)
{
    char full[128]; uint32_t p = 0;
    fb_puts(full, sizeof(full), &p, "src_grep ");
    fb_puts(full, sizeof(full), &p, args);
    src_tool(full, out_buf, max_out);
    return 1;
}

static int cmd_hpatch(const char *args, char *out_buf, uint32_t max_out)
{
    (void)out_buf; (void)max_out;
    hpatch_command(args);
    return 1;
}

static int cmd_panic(const char *args, char *out_buf, uint32_t max_out)
{
    (void)out_buf; (void)max_out;
    while (*args == ' ') args++;
    if (args[0] == 'd' && args[1] == 'i' && args[2] == 'v') {
        kprint("Provocando division por cero (#DE)...\n");
        volatile int z = 0; volatile int x = 42 / z; (void)x;
    } else if (args[0] == 'u' && args[1] == 'd') {
        kprint("Ejecutando instruccion invalida (#UD)...\n");
#ifdef __x86_64__
        __asm__ volatile ("ud2");
#else
        __builtin_trap();
#endif
    } else {
        kprint("Provocando fallo de pagina (#PF) en 0xDEADBEEF00...\n");
        volatile uint64_t *bad = (volatile uint64_t *)0xDEADBEEF00ULL; *bad = 0xCAFEBABE;
    }
    return 1;
}

static int cmd_env(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    if (!env_initialized) env_init_defaults();
    uint32_t pos = 0;
    kprint("\nVARIABLES DE ENTORNO ACTIVAS:\n------------------------------\n");
    for (int i = 0; i < MAX_ENV_VARS; ++i) {
        if (env_table[i].used) {
            kprint("  "); kprint(env_table[i].key); kputc('='); kprint(env_table[i].val); kprint("\n");
            fb_puts(out_buf, max_out, &pos, env_table[i].key);
            fb_putc(out_buf, max_out, &pos, '=');
            fb_puts(out_buf, max_out, &pos, env_table[i].val);
            fb_putc(out_buf, max_out, &pos, '\n');
        }
    }
    kprint("------------------------------\n");
    return 1;
}

static int cmd_export(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        return cmd_env(args, out_buf, max_out);
    }
    char k[ENV_KEY_LEN], v[ENV_VAL_LEN];
    uint32_t kp = 0, vp = 0;
    while (*args && *args != '=' && *args != ' ' && kp < sizeof(k) - 1) k[kp++] = *args++;
    k[kp] = '\0';
    if (*args == '=' || *args == ' ') args++;
    while (*args == ' ') args++;

    char q = 0;
    if (*args == '"' || *args == '\'') {
        q = *args++;
    }
    while (*args && vp < sizeof(v) - 1) {
        if (q && *args == q) { args++; break; }
        v[vp++] = *args++;
    }
    v[vp] = '\0';

    if (kp > 0) {
        env_set(k, v);
        kprint("export: "); kprint(k); kputc('='); kprint(v); kprint("\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, k); fb_putc(out_buf, max_out, &p, '='); fb_puts(out_buf, max_out, &p, v);
        return 1;
    }
    kprint("Uso: export VAR=valor\n");
    return 1;
}

static int cmd_unset(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: unset <VAR>\n");
        return 1;
    }
    env_unset(args);
    kprint("unset: "); kprint(args); kprint("\n");
    uint32_t p = 0; fb_puts(out_buf, max_out, &p, "unset OK");
    return 1;
}

#define SOURCE_MAX_DEPTH 3

static int cmd_source(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: source <archivo.sh>  (alias: sh <archivo.sh>)\n");
        return 1;
    }

    /* PARCHE 046: profundidad por hilo + buffer propio (antes: static compartido). */
    struct tcb *self = thread_current();
    static int source_depth_fallback = 0;
    int *sdepth = self ? &self->source_depth : &source_depth_fallback;
    uint32_t pos = 0;

    if (*sdepth >= SOURCE_MAX_DEPTH) {
        kprint("source: anidamiento maximo alcanzado (3).\n");
        fb_puts(out_buf, max_out, &pos, "Error: source anidado en exceso");
        return 0;
    }

    char *script_buf = (char *)kmalloc(4096);
    if (!script_buf) {
        kprint("source: sin memoria.\n");
        fb_puts(out_buf, max_out, &pos, "Error: sin memoria");
        return 0;
    }

    int r = vfs_read(args, script_buf, 4095);
    if (r < 0) {
        kprint("source: archivo no encontrado: '"); kprint(args); kprint("'\n");
        fb_puts(out_buf, max_out, &pos, "source: archivo no encontrado");
        kfree(script_buf);
        return 1;
    }
    script_buf[r] = '\0';

    (*sdepth)++;
    const char *p = script_buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') { p++; continue; }
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        char line[256];
        uint32_t lp = 0;
        while (*p && *p != '\n' && lp < sizeof(line) - 1) {
            if (*p != '\r') line[lp++] = *p;
            p++;
        }
        line[lp] = '\0';
        if (*p == '\n') p++;

        while (lp > 0 && (line[lp - 1] == ' ' || line[lp - 1] == '\t')) {
            line[--lp] = '\0';
        }

        if (lp > 0) {
            char sub_out[1024];
            sub_out[0] = '\0';
            dispatch_command(line, sub_out, sizeof(sub_out));
        }
    }
    (*sdepth)--;
    kfree(script_buf);

    fb_puts(out_buf, max_out, &pos, "source: ejecucion completada");
    return 1;
}

/* ====================================================================
 * TABLA DECLARATIVA CANÓNICA DE COMANDOS DE SOMA
 * ==================================================================== */

static int cmd_history(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    return console_history_dump(out_buf, max_out);
}

/* PARCHE 046: banco de pruebas de la politica del Agente (solo usuario). */
static int cmd_agent_sim(const char *args, char *out_buf, uint32_t max_out)
{
    char cmd[256];
    uint32_t n = 0;
    char q = 0;

    while (*args == ' ') args++;
    if (*args == '\0') {
        kprint("Uso: agent_sim <comando>   (usa comillas si contiene ; | > &&)\n");
        return 1;
    }
    if (*args == '"' || *args == '\'') q = *args++;
    while (*args && n < sizeof(cmd) - 1) {
        if (q && *args == q && args[1] == '\0') break;
        cmd[n++] = *args++;
    }
    cmd[n] = '\0';

    int r = dispatch_agent_command(cmd, out_buf, max_out);
    kprint("[agent_sim] rc="); kprint_dec(r ? 1 : 0); kprint("\n");
    return 1;
}

static const struct command_entry cmd_table[] = {
    /* Scripting y Automatización */
    { "history",       cmd_history,      CMD_F_SAFE,   "Muestra el historial persistente de comandos" },
    { "source",       cmd_source,       CMD_F_SAFE,   "Ejecuta un script de comandos por lotes (.sh)" },
    { "sh",           cmd_source,       CMD_F_ALIAS,  "Alias de source" },

    /* Sistema, Ayuda e Identidad */
    { "help",         cmd_help,         CMD_F_SAFE,   "Muestra este catalogo estructurado de comandos" },
    { "somafetch",    cmd_somafetch,    CMD_F_SAFE,   "Ficha del sistema y telemetria en arte ASCII" },
    { "neofetch",     cmd_somafetch,    CMD_F_ALIAS,  "Alias de somafetch" },
    { "fetch",        cmd_somafetch,    CMD_F_ALIAS,  "Alias de somafetch" },
    { "creador",      cmd_creador,      CMD_F_SAFE,   "Muestra informacion de la autoría de SOMA" },
    { "status",       cmd_status,       CMD_F_SAFE,   "Muestra estado basico de red e IP" },
    { "uptime",       cmd_uptime,       CMD_F_SAFE,   "Muestra el tiempo de actividad del kernel" },
    { "date",         cmd_date,         CMD_F_SAFE,   "Fecha y hora real UTC desde el reloj CMOS RTC" },
    { "clear",        cmd_clear,        CMD_F_SAFE,   "Limpia el buffer de pantalla VGA" },
    { "cls",          cmd_clear,        CMD_F_ALIAS,  "Alias de clear" },

    /* Variables de Entorno */
    { "env",          cmd_env,          CMD_F_SAFE,   "Muestra variables de entorno activas" },
    { "export",       cmd_export,       CMD_F_SAFE,   "Define o modifica una variable de entorno (VAR=val)" },
    { "unset",        cmd_unset,        CMD_F_SAFE,   "Elimina una variable de entorno activa" },

    /* Diagnóstico, Telemetría y Memoria */
    { "test_suite",   cmd_test_suite,   CMD_F_SAFE,   "Suite integral de auto-test y no-regresion (9 fases)" },
    { "test",         cmd_test_suite,   CMD_F_ALIAS,  "Alias de test_suite" },
    { "mem",          cmd_mem,          CMD_F_SAFE,   "Inspeccion fisica de CR0, CR3, CR4 y paginacion" },
    { "free",         cmd_free,         CMD_F_SAFE,   "Memoria RAM fisica (PMM) y KHeap MMU libre/usado" },
    { "df",           cmd_df,           CMD_F_SAFE,   "Capacidad de almacenamiento y estado de RamFS" },
    { "stats",        cmd_stats,        CMD_F_SAFE,   "Estadisticas de paquetes y bytes VirtIO-NET" },
    { "heap",         cmd_heap,         CMD_F_SAFE,   "Telemetria y auto-test del KHeap dinamico" },
    { "pmm",          cmd_pmm,          CMD_F_SAFE,   "Telemetria y auto-test del PMM bitmap allocator" },
    { "vmm",          cmd_vmm,          CMD_F_SAFE,   "Verificacion de tablas de paginas MMU 4 KiB" },
    { "pci",          cmd_pci,          CMD_F_SAFE,   "Escaneo completo del bus PCI y BAR0 I/O" },
    { "arp",          cmd_arp,          CMD_F_SAFE,   "Tabla de cache de resolucion dinamica ARP" },

    /* Multitarea, Hilos y Control de Energía */
    { "ps",           cmd_ps,           CMD_F_SAFE,   "Tabla de hilos de kernel, demonios y ticks CPU" },
    { "threads",      cmd_threads,      CMD_F_SAFE,   "Supervision de KThreads y stacks virtuales" },
    { "spawn",        cmd_spawn,        CMD_F_SAFE,   "Lanza un comando como hilo en segundo plano" },
    { "bg",           cmd_spawn,        CMD_F_ALIAS,  "Alias de spawn" },
    { "kill",         cmd_kill,         CMD_F_NORMAL, "Termina un hilo en segundo plano (TID > 3)" },
    { "sleep",        cmd_sleep,        CMD_F_SAFE,   "Suspende la CPU con HLT durante N milisegundos" },
    { "time",         cmd_time,         CMD_F_SAFE,   "Mide milisegundos y ciclos TSC de un comando" },
    { "reboot",       cmd_reboot,       CMD_F_NORMAL, "Reinicia fisicamente el equipo (8042 reset)" },
    { "poweroff",     cmd_poweroff,     CMD_F_NORMAL, "Apaga el sistema limpiamente via ACPI" },
    { "shutdown",     cmd_poweroff,     CMD_F_ALIAS,  "Alias de poweroff" },
    { "halt",         cmd_halt,         CMD_F_NORMAL, "Sincroniza discos y detiene la CPU (HLT)" },

    /* Sistema de Archivos y Coreutils */
    { "ls",           cmd_ls,           CMD_F_SAFE,   "Lista archivos persistentes del RamFS" },
    { "cat",          cmd_cat,          CMD_F_SAFE,   "Muestra el contenido de un archivo" },
    { "write",        cmd_write,        CMD_F_SAFE,   "Crea o sobrescribe un archivo en RamFS" },
    { "rm",           cmd_rm,           CMD_F_SAFE,   "Elimina un archivo del RamFS" },
    { "cp",           cmd_cp,           CMD_F_SAFE,   "Copia un archivo a una nueva ruta" },
    { "mv",           cmd_mv,           CMD_F_SAFE,   "Mueve o renombra un archivo en RamFS" },
    { "touch",        cmd_touch,        CMD_F_SAFE,   "Crea un archivo vacio si no existe" },
    { "tree",         cmd_tree,         CMD_F_SAFE,   "Muestra la jerarquia de archivos en arbol" },
    { "head",         cmd_head,         CMD_F_SAFE,   "Muestra las primeras N lineas de un archivo" },
    { "tail",         cmd_tail,         CMD_F_SAFE,   "Muestra las ultimas N lineas de un archivo" },
    { "wc",           cmd_wc,           CMD_F_SAFE,   "Cuenta lineas, palabras y bytes de un archivo" },
    { "grep",         cmd_grep,         CMD_F_SAFE,   "Busca patrones de texto dentro de un archivo" },
    { "echo",         cmd_echo,         CMD_F_SAFE,   "Imprime texto en consola o con redireccion" },
    { "nano",         cmd_nano,         CMD_F_NORMAL, "Editor de texto visual interactivo en 80x25" },
    { "edit",         cmd_nano,         CMD_F_ALIAS,  "Alias de nano" },
    { "hexdump",      cmd_hexdump,      CMD_F_SAFE,   "Volcado canonico hexadecimal y ASCII de archivo" },
    { "xxd",          cmd_hexdump,      CMD_F_ALIAS,  "Alias de hexdump" },

    /* Almacenamiento Persistente a Bajo Nivel */
    { "checkpoint",   cmd_checkpoint,   CMD_F_SAFE,   "Guarda snapshot persistente en virtio-blk (LBA 4096)" },
    { "fs-backup",    cmd_checkpoint,   CMD_F_ALIAS,  "Alias de checkpoint" },
    { "rollback",     cmd_rollback,     CMD_F_NORMAL, "Restaura RamFS desde el snapshot fisico" },
    { "fs-restore",   cmd_rollback,     CMD_F_ALIAS,  "Alias de rollback" },
    { "fs-sync",      cmd_fs_sync,      CMD_F_SAFE,   "Fuerza sincronizacion de inodos sucios a disco" },
    { "fs-format",    cmd_fs_format,    CMD_F_NORMAL, "Formatea RamFS restaurando imagen de fabrica" },
    { "sector_read",  cmd_sector_read,  CMD_F_SAFE,   "Lectura fisica directa de sector LBA de 512 B" },
    { "sector_r",     cmd_sector_read,  CMD_F_ALIAS,  "Alias de sector_read" },
    { "sector_write", cmd_sector_write, CMD_F_NORMAL, "Escritura fisica directa en sector LBA de usuario" },
    { "sector_w",     cmd_sector_write, CMD_F_ALIAS,  "Alias de sector_write" },

    /* Red TCP/IP y Web */
    { "httpd",        cmd_httpd,        CMD_F_SAFE,   "Estado del servidor web HTTP (puerto 80 / localhost:8090)" },
    { "ping",         cmd_ping,         CMD_F_SAFE,   "Envia peticiones ICMP Echo a una IP destino" },
    { "dns",          cmd_dns,          CMD_F_SAFE,   "Resolucion dinamica de nombres de dominio DNS" },
    { "curl",         cmd_curl,         CMD_F_SAFE,   "Cliente HTTP/1.1 con resolucion y cabeceras" },

    /* Inteligencia Artificial y Agente Autónomo */
    { "soma",         cmd_soma,         CMD_F_NORMAL, "Invoca al Agente IA ReAct con memoria persistente" },
    { "myos",         cmd_soma,         CMD_F_ALIAS,  "Alias de soma" },
    { "agent",        cmd_soma,         CMD_F_ALIAS,  "Alias de soma" },
    { "health",       cmd_health,       CMD_F_SAFE,   "Verifica estado del servidor LLM local (/health)" },
    { "llm",          cmd_llm,          CMD_F_NORMAL, "Consulta libre en lenguaje natural a nail-35b" },
    { "llm-diag",     cmd_llm_diag,     CMD_F_SAFE,   "Diagnostico holistico del kernel mediante IA" },

    /* Inspección de Código Fuente del Kernel (SRCFS) */
    { "src_ls",       cmd_src_ls,       CMD_F_SAFE,   "Lista archivos fuente del kernel empaquetados" },
    { "src_cat",      cmd_src_cat,      CMD_F_SAFE,   "Muestra un fragmento de codigo fuente desde offset" },
    { "src_grep",     cmd_src_grep,     CMD_F_SAFE,   "Busca cadenas en los fuentes con linea y offset" },

    /* Auto-Modificación y Pruebas Críticas */
    { "hpatch",       cmd_hpatch,       CMD_F_NORMAL, "Prueba del buzon de parches y Singularity Loop" },
    { "panic",        cmd_panic,        CMD_F_NORMAL, "Dispara excepciones de CPU para verificar IDT/TSS" },
    { "agent_sim",    cmd_agent_sim,    CMD_F_NORMAL, "Ejecuta un comando bajo la politica del Agente (pruebas de seguridad)" },
};

static const int cmd_table_count = sizeof(cmd_table) / sizeof(cmd_table[0]);

static int cmd_help(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    kprint("Comandos disponibles en SOMA 0.2:\n");
    kprint("----------------------------------------------------------------------\n");
    uint32_t pos = 0;

    for (int i = 0; i < cmd_table_count; ++i) {
        if (cmd_table[i].flags & CMD_F_ALIAS) continue;

        char line_buf[128]; uint32_t lp = 0;
        fb_puts(line_buf, sizeof(line_buf), &lp, "  ");
        fb_puts(line_buf, sizeof(line_buf), &lp, cmd_table[i].name);
        uint32_t nl = 0; while (cmd_table[i].name[nl]) nl++;
        for (uint32_t s = nl; s < 18; ++s) fb_putc(line_buf, sizeof(line_buf), &lp, ' ');
        fb_puts(line_buf, sizeof(line_buf), &lp, "- ");
        fb_puts(line_buf, sizeof(line_buf), &lp, cmd_table[i].help);
        fb_puts(line_buf, sizeof(line_buf), &lp, "\n");

        kprint(line_buf);
        if (pos + lp < max_out - 1) {
            fb_puts(out_buf, max_out, &pos, line_buf);
        }
    }
    kprint("----------------------------------------------------------------------\n");
    return 1;
}

int shell_autocomplete(const char *prefix, uint32_t plen, const char *matches[], int max_matches)
{
    int count = 0;
    for (int i = 0; i < cmd_table_count; ++i) {
        const char *name = cmd_table[i].name;
        int match = 1;
        for (uint32_t j = 0; j < plen; ++j) {
            if (name[j] != prefix[j]) { match = 0; break; }
        }
        if (match && count < max_matches) {
            matches[count++] = name;
        }
    }
    return count;
}

/* PARCHE 046: limite de anidamiento (evita desbordar la pila de 32 KiB de los
 * hilos con 'a;b;c;...' o 'a&&b&&...'). El limite cuenta niveles de recursion. */
#define DISPATCH_MAX_DEPTH 8
static int dispatch_depth_fallback = 0;

static int dispatch_command_inner(const char *cmd_line, char *out_buf, uint32_t max_out);

int dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out)
{
    struct tcb *self = thread_current();
    int *depth = self ? &self->dispatch_depth : &dispatch_depth_fallback;

    if (*depth >= DISPATCH_MAX_DEPTH) {
        console_muted = 0;
        kprint("\n[SEGURIDAD] Anidamiento de comandos excesivo (max 8): abortado.\n");
        if (out_buf && max_out) {
            uint32_t ep = 0;
            fb_puts(out_buf, max_out, &ep, "Error: anidamiento de comandos excesivo");
        }
        return 0;
    }

    (*depth)++;
    int r = dispatch_command_inner(cmd_line, out_buf, max_out);
    (*depth)--;
    return r;
}

static int dispatch_command_inner(const char *cmd_line, char *out_buf, uint32_t max_out)
{
    static int redir_active = 0;
    static char redir_buf[4096];
    static char pipe_buf[4096];
    char expanded_cmd[512];

    if (!cmd_line) return 0;
    while (*cmd_line == ' ') cmd_line++;
    if (*cmd_line == '\0') return 0;

    /* 0. Interceptores Lógicos Condicionales ('&&' y '||') */
    int logic_and_idx = -1;
    int logic_or_idx = -1;
    char l_quote = 0;
    for (int i = 0; cmd_line[i] && cmd_line[i + 1]; ++i) {
        char c = cmd_line[i];
        if (c == '"' || c == '\'') {
            if (l_quote == 0) l_quote = c;
            else if (l_quote == c) l_quote = 0;
        } else if (!l_quote) {
            if (c == '&' && cmd_line[i + 1] == '&') {
                logic_and_idx = i;
                break;
            }
            if (c == '|' && cmd_line[i + 1] == '|') {
                logic_or_idx = i;
                break;
            }
        }
    }

    if (logic_and_idx >= 0) {
        char left_cmd[256];
        int lp = 0;
        for (int i = 0; i < logic_and_idx && lp < 255; ++i) left_cmd[lp++] = cmd_line[i];
        while (lp > 0 && left_cmd[lp - 1] == ' ') lp--;
        left_cmd[lp] = '\0';

        const char *right_cmd = cmd_line + logic_and_idx + 2;
        while (*right_cmd == ' ') right_cmd++;

        int ok = dispatch_command(left_cmd, out_buf, max_out);
        if (ok && *right_cmd != '\0') {
            return dispatch_command(right_cmd, out_buf, max_out);
        }
        return ok;
    }

    if (logic_or_idx >= 0) {
        char left_cmd[256];
        int lp = 0;
        for (int i = 0; i < logic_or_idx && lp < 255; ++i) left_cmd[lp++] = cmd_line[i];
        while (lp > 0 && left_cmd[lp - 1] == ' ') lp--;
        left_cmd[lp] = '\0';

        const char *right_cmd = cmd_line + logic_or_idx + 2;
        while (*right_cmd == ' ') right_cmd++;

        int ok = dispatch_command(left_cmd, out_buf, max_out);
        if (!ok && *right_cmd != '\0') {
            return dispatch_command(right_cmd, out_buf, max_out);
        }
        return ok;
    }

    /* 1. Interceptor de Comandos Encadenados (';') ANTES de expandir $VAR en bloque */
    int semi_idx = -1;
    char s_quote = 0;
    for (int i = 0; cmd_line[i]; ++i) {
        char c = cmd_line[i];
        if (c == '"' || c == '\'') {
            if (s_quote == 0) s_quote = c;
            else if (s_quote == c) s_quote = 0;
        } else if (!s_quote && c == ';') {
            semi_idx = i;
            break;
        }
    }
    if (semi_idx >= 0) {
        char first_cmd[256];
        int f_p = 0;
        for (int i = 0; i < semi_idx && f_p < 255; ++i) first_cmd[f_p++] = cmd_line[i];
        while (f_p > 0 && first_cmd[f_p - 1] == ' ') f_p--;
        first_cmd[f_p] = '\0';

        const char *next_cmd = cmd_line + semi_idx + 1;
        while (*next_cmd == ' ') next_cmd++;

        if (f_p > 0) {
            dispatch_command(first_cmd, out_buf, max_out);
        }
        if (*next_cmd != '\0') {
            return dispatch_command(next_cmd, out_buf, max_out);
        }
        return 1;
    }

    /* 2. Expansión dinámica de variables de entorno ($VAR) para este comando atómico */
    env_expand(cmd_line, expanded_cmd, sizeof(expanded_cmd));
    cmd_line = expanded_cmd;
    while (*cmd_line == ' ') cmd_line++;

    /* 3. Interceptor de Tuberías Bare-Metal ('|') */
    int pipe_idx = -1;
    char p_quote = 0;
    for (int i = 0; cmd_line[i]; ++i) {
        char c = cmd_line[i];
        if (c == '"' || c == '\'') {
            if (p_quote == 0) p_quote = c;
            else if (p_quote == c) p_quote = 0;
        } else if (!p_quote && c == '|') {
            pipe_idx = i;
            break;
        }
    }
    if (pipe_idx >= 0) {
        char left_cmd[256], right_cmd[256];
        int lp = 0, rp = 0;
        for (int i = 0; i < pipe_idx && lp < 255; ++i) left_cmd[lp++] = cmd_line[i];
        while (lp > 0 && left_cmd[lp - 1] == ' ') lp--;
        left_cmd[lp] = '\0';

        const char *r_ptr = cmd_line + pipe_idx + 1;
        while (*r_ptr == ' ') r_ptr++;
        while (*r_ptr && rp < 255) right_cmd[rp++] = *r_ptr++;
        right_cmd[rp] = '\0';

        if (lp > 0 && rp > 0) {
            pipe_buf[0] = '\0';
            console_muted = 1;
            int r_left = dispatch_command(left_cmd, pipe_buf, sizeof(pipe_buf));
            console_muted = 0;

            if (r_left) {
                pipe_stdin_data = pipe_buf;
                pipe_stdin_len = 0;
                while (pipe_buf[pipe_stdin_len]) pipe_stdin_len++;

                int r_right = dispatch_command(right_cmd, out_buf, max_out);
                pipe_stdin_data = 0;
                pipe_stdin_len = 0;
                return r_right;
            }
        }
        return 0;
    }

    /* 4. Interceptor de Redirección Universal ('>') */
    if (!redir_active) {
        int gt_idx = -1;
        char active_quote = 0;
        for (int i = 0; cmd_line[i]; ++i) {
            char c = cmd_line[i];
            if (c == '"' || c == '\'') {
                if (active_quote == 0) active_quote = c;
                else if (active_quote == c) active_quote = 0;
            } else if (!active_quote && c == '>') {
                int prev_space = (i > 0 && cmd_line[i - 1] == ' ');
                int next_space = (cmd_line[i + 1] == ' ' || cmd_line[i + 1] == '/');
                if (prev_space && next_space) { gt_idx = i; break; }
            }
        }
        if (gt_idx >= 0) {
            char sub_cmd[256], dst_file[64];
            int sp = 0, dp = 0;
            for (int i = 0; i < gt_idx && sp < 255; ++i) sub_cmd[sp++] = cmd_line[i];
            while (sp > 0 && sub_cmd[sp - 1] == ' ') sp--;
            sub_cmd[sp] = '\0';
            const char *p = cmd_line + gt_idx + 1;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && dp < 63) dst_file[dp++] = *p++;
            dst_file[dp] = '\0';

            if (sp > 0 && dp > 0) {
                redir_active = 1; console_muted = 1; redir_buf[0] = '\0';
                int res = dispatch_command(sub_cmd, redir_buf, sizeof(redir_buf));
                console_muted = 0; redir_active = 0;
                if (res) {
                    uint32_t rlen = 0; while (redir_buf[rlen]) rlen++;
                    if (vfs_write(dst_file, redir_buf, rlen) < 0) {
                        kprint("Error: redireccion fallida hacia '"); kprint(dst_file);
                        kprint("' (acceso denegado o fallo de disco).\n");
                        uint32_t epos = 0;
                        fb_puts(out_buf, max_out, &epos, "Error: redireccion denegada o fallida");
                        return 1;
                    }
                    kprint("Salida redirigida con exito a '"); kprint(dst_file); kprint("' (");
                    kprint_dec(rlen); kprint(" bytes)\n");
                    uint32_t pos = 0;
                    fb_puts(out_buf, max_out, &pos, "Salida redirigida a ");
                    fb_puts(out_buf, max_out, &pos, dst_file);
                    return 1;
                }
            }
        }
    }

    /* 5. Extraer token del nombre del comando */
    char cmd_name[32];
    uint32_t ci = 0;
    const char *p = cmd_line;
    while (*p && *p != ' ' && ci < sizeof(cmd_name) - 1) {
        cmd_name[ci++] = *p++;
    }
    cmd_name[ci] = '\0';
    while (*p == ' ') p++;

    /* 6. Búsqueda canónica en la tabla declarativa */
    for (int i = 0; i < cmd_table_count; ++i) {
        if (str_eq(cmd_table[i].name, cmd_name)) {
            /* AGENT_POLICY_V1: el agente sólo puede usar CMD_F_SAFE. */
            if (agent_dispatch_mode && !(cmd_table[i].flags & CMD_F_SAFE)) {
                kprint("\n[SEGURIDAD AGENTE] Comando bloqueado: '");
                kprint(cmd_table[i].name);
                kprint("' no esta autorizado para el Agente ReAct.\n");

                uint32_t denied_pos = 0;
                fb_puts(out_buf, max_out, &denied_pos,
                        "AGENT_DENIED: comando no autorizado");
                return 0;
            }

            return cmd_table[i].handler(p, out_buf, max_out);
        }
    }

    /* 7. Fallback para utilidades SRCFS por compatibilidad */
    if (src_tool(cmd_line, out_buf, max_out)) {
        return 1;
    }

    return 0;
}

void shell_run(void)
{
    char cmd[512];

    for (;;) {
        if (agent_resume_pending == 1) {
            const char *pre = "soma "; uint32_t ci = 0;
            while (*pre) cmd[ci++] = *pre++;
            for (uint32_t k = 0; agent_resume_mission[k] && ci < sizeof(cmd) - 1; ++k) cmd[ci++] = agent_resume_mission[k];
            cmd[ci] = '\0';
            kprint("\033[1;36msoma\033[1;32m>\033[0m "); kprint(cmd); kprint(" [reanudacion automatica]\n");
        } else {
            kprint("\033[1;36msoma\033[1;32m>\033[0m ");
            kgetline(cmd, sizeof(cmd));
        }

        char *s0 = cmd; while (*s0 == ' ') s0++;
        if (*s0 == '\0') continue;

        static char shell_buf[2048];
        if (!dispatch_command(s0, shell_buf, sizeof(shell_buf))) {
            kprint("Comando desconocido: '"); kprint(s0); kprint("'. Escribe 'help'.\n");
        }
    }
}
