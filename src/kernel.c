#include <stdint.h>
#include "pci.h"
#include "virtio_net.h"
#include "virtio_blk.h"
#include "net.h"
#include "ip.h"
#include "dns.h"
#include "tcp.h"
#include "http.h"
#include "llm.h"
#include "console.h"
#include "sysinfo.h"
#include "mem.h"
#include "fs.h"
#include "io.h"
#include "srcfs.h"
#include "idt.h"
#include "boot_gate.h"

static volatile uint16_t *const VGA =
    (uint16_t *)0xB8000;

#define COM1 0x3F8

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_ready(void)
{
    return (inb(COM1 + 5) & 0x20) != 0;
}

static void serial_putc(char c)
{
    kputc(c);
}

static void serial_print(const char *s)
{
    for (; *s; ++s) {
        serial_putc(*s);
    }
}

static void serial_put_hex4(uint8_t value)
{
    value &= 0x0F;

    if (value < 10) {
        serial_putc('0' + value);
    } else {
        serial_putc('A' + (value - 10));
    }
}

static void serial_put_hex16(uint16_t value)
{
    for (int shift = 12; shift >= 0; shift -= 4) {
        serial_put_hex4(
            (uint8_t)(value >> shift)
        );
    }
}

static void serial_put_hex32(uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_put_hex4(
            (uint8_t)(value >> shift)
        );
    }
}

static void serial_put_dec(uint8_t value)
{
    if (value >= 100) {
        serial_putc('0' + value / 100);
        value %= 100;
        serial_putc('0' + value / 10);
        serial_putc('0' + value % 10);
        return;
    }

    if (value >= 10) {
        serial_putc('0' + value / 10);
        serial_putc('0' + value % 10);
        return;
    }

    serial_putc('0' + value);
}

static void clear_screen(void)
{
    console_clear();
}

static void vga_print_at(
    const char *s,
    uint32_t row,
    uint32_t col
)
{
    uint32_t pos = row * 80 + col;

    for (uint32_t i = 0;
         s[i] != '\0' && pos < 80 * 25;
         ++i, ++pos)
    {
        VGA[pos] =
            ((uint16_t)0x07 << 8) |
            (uint8_t)s[i];
    }
}

static void pci_scan(void)
{
    serial_print("\nPCI SCAN\n");
    serial_print("--------\n");

    uint32_t devices_found = 0;
    uint32_t virtio_found = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {

        for (uint8_t slot = 0; slot < 32; ++slot) {

            for (uint8_t function = 0;
                 function < 8;
                 ++function)
            {
                uint16_t vendor =
                    pci_vendor_id(
                        (uint8_t)bus,
                        slot,
                        function
                    );

                if (vendor == 0xFFFF) {
                    continue;
                }

                uint16_t device =
                    pci_device_id(
                        (uint8_t)bus,
                        slot,
                        function
                    );

                ++devices_found;

                serial_print("PCI ");

                if (bus < 16) {
                    serial_putc('0');
                }

                serial_put_hex16(
                    (uint16_t)bus
                );

                serial_putc(':');

                if (slot < 10) {
                    serial_putc('0');
                }

                serial_put_hex4(slot);

                serial_putc('.');

                serial_put_dec(function);

                serial_print(
                    " vendor=0x"
                );

                serial_put_hex16(vendor);

                serial_print(
                    " device=0x"
                );

                serial_put_hex16(device);

                if (vendor == 0x1AF4 &&
                    (device == 0x1041 ||
                     device == 0x1000))
                {
                    serial_print(
                        "  <-- VIRTIO-NET"
                    );

                    uint32_t bar0 =
                        pci_bar0(
                            (uint8_t)bus,
                            slot,
                            function
                        );

                    serial_print("\n");

                    serial_print(
                        "    BAR0 raw = 0x"
                    );

                    serial_put_hex32(bar0);

                    serial_print("\n");

                    if (bar0 & 0x1) {

                        uint32_t io_base =
                            bar0 & 0xFFFFFFFC;

                        serial_print(
                            "    BAR0 type = I/O\n"
                        );

                        serial_print(
                            "    I/O base  = 0x"
                        );

                        serial_put_hex32(
                            io_base
                        );

                        serial_print("\n");

                    } else {

                        serial_print(
                            "    BAR0 type = MEMORY\n"
                        );
                    }

                    ++virtio_found;
                }

                serial_print("\n");
            }
        }
    }

    serial_print("\nPCI devices found: ");

    if (devices_found > 999) {
        serial_print(">999");
    } else {
        serial_put_dec(
            (uint8_t)devices_found
        );
    }

    serial_print("\nVirtIO-NET found: ");

    if (virtio_found != 0) {
        serial_print("YES");
    } else {
        serial_print("NO");
    }

    serial_print("\n");
}



static uint64_t parse_num(const char **str) {
    uint64_t val = 0;
    while (**str == ' ') (*str)++;
    while (**str >= '0' && **str <= '9') {
        val = val * 10 + (**str - '0');
        (*str)++;
    }
    return val;
}

static void parse_ip(const char *s, uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        ip[i] = (uint8_t)val;
        if (*s == '.') s++;
    }
}

static const char *find_substr(const char *haystack, const char *needle)
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

static void fb_putc(char *out, uint32_t max, uint32_t *pos, char c)
{
    if (*pos + 1 < max) {
        out[(*pos)++] = c;
        out[*pos] = '\0';
    }
}

static void fb_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    while (*s && *pos + 1 < max) {
        out[(*pos)++] = *s++;
    }
    out[*pos] = '\0';
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

static void fb_put_dec(char *out, uint32_t max, uint32_t *pos, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) { fb_putc(out, max, pos, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) fb_putc(out, max, pos, tmp[--n]);
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
    fb_puts(out, max, &pos, "Total: ");
    fb_put_dec(out, max, &pos, count);
    fb_puts(out, max, &pos, " dispositivos.\n");
    return (int)pos;
}

static uint32_t max_fb_dummy = 2048;

/* ---- Despachador unificado de comandos (Shell y Agente) ---------- */
static int dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out)
{
    while (*cmd_line == ' ') cmd_line++;

    if (cmd_line[0] == 'c' && cmd_line[1] == 'r' && cmd_line[2] == 'e' && cmd_line[3] == 'a' && cmd_line[4] == 'd' && cmd_line[5] == 'o' && cmd_line[6] == 'r') {
        kprint("MYOS - Creado por Marcos\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "MYOS - Creado por Marcos");
        return 1;
    }

    if (cmd_line[0] == 'u' && cmd_line[1] == 'p' && cmd_line[2] == 't' && cmd_line[3] == 'i' && cmd_line[4] == 'm' && cmd_line[5] == 'e') {
        uint64_t ms = timer_get_uptime_ms();
        uint32_t s = (uint32_t)(ms / 1000);
        uint32_t m = s / 60; s %= 60;
        uint32_t h = m / 60; m %= 60;
        kprint("Uptime del sistema: ");
        if (h > 0) { kprint_dec(h); kprint("h "); }
        if (m > 0 || h > 0) { kprint_dec(m); kprint("m "); }
        kprint_dec(s); kprint("s (");
        kprint_dec((uint32_t)ms); kprint(" ms totales)\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "Uptime: ");
        fb_put_dec(out_buf, max_out, &p, (uint32_t)(ms / 1000));
        fb_puts(out_buf, max_out, &p, " segundos.");
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'l' && cmd_line[2] == 'e' && cmd_line[3] == 'e' && cmd_line[4] == 'p' && cmd_line[5] == ' ') {
        const char *arg = cmd_line + 6;
        while (*arg == ' ') arg++;
        uint32_t ms = 0;
        while (*arg >= '0' && *arg <= '9') { ms = ms * 10 + (*arg++ - '0'); }
        if (ms == 0) ms = 1000;
        kprint("Durmiendo "); kprint_dec(ms); kprint(" ms con HLT...\n");
        timer_sleep_ms(ms);
        kprint("Despierto.\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "Sleep completado.");
        return 1;
    } else if (cmd_line[0] == 'd' && cmd_line[1] == 'n' && cmd_line[2] == 's' && cmd_line[3] == ' ') {
        const char *name = cmd_line + 4;
        while (*name == ' ') name++;
        char domain[64]; uint32_t di = 0;
        while (*name && *name != ' ' && di < sizeof(domain) - 1) domain[di++] = *name++;
        domain[di] = '\0';
        uint8_t ip[4];
        kprint("DNS resolviendo '"); kprint(domain); kprint("'...\n");
        if (dns_resolve(domain, ip, 3000)) {
            kprint("IP resuelta: "); kprint_ip(ip); kprint("\n");
            uint32_t p = 0;
            fb_puts(out_buf, max_out, &p, "DNS ");
            fb_puts(out_buf, max_out, &p, domain);
            fb_puts(out_buf, max_out, &p, " -> ");
            for (int k = 0; k < 4; ++k) {
                fb_put_dec(out_buf, max_out, &p, ip[k]);
                if (k != 3) fb_putc(out_buf, max_out, &p, '.');
            }
        } else {
            kprint("Fallo resolucion DNS.\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Error resolviendo DNS.");
        }
        return 1;
    } else if (cmd_line[0] == 'c' && cmd_line[1] == 'u' && cmd_line[2] == 'r' && cmd_line[3] == 'l' && cmd_line[4] == ' ') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char target[64];
        uint32_t ti = 0;
        while (*p && *p != ' ' && ti < sizeof(target) - 1) {
            target[ti++] = *p++;
        }
        target[ti] = '\0';
        while (*p == ' ') p++;

        if (ti == 0) {
            kprint("Uso: curl <dominio|ip> [puerto] [ruta]\n");
            return 1;
        }

        uint16_t port = 80;
        if (*p >= '0' && *p <= '9') {
            port = 0;
            while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
        }
        while (*p == ' ') p++;
        const char *path = (*p) ? p : "/";

        int is_ip = 1;
        int dots = 0;
        for (int i = 0; target[i]; ++i) {
            if (target[i] == '.') dots++;
            else if (target[i] < '0' || target[i] > '9') is_ip = 0;
        }
        if (dots != 3) is_ip = 0;

        uint8_t tip[4];
        const char *host_hdr = 0;

        if (is_ip) {
            parse_ip(target, tip);
        } else {
            kprint("DNS resolviendo '"); kprint(target); kprint("'...\n");
            if (!dns_resolve(target, tip, 3000)) {
                kprint("Error: no se pudo resolver el dominio '"); kprint(target); kprint("'\n");
                uint32_t pos = 0;
                fb_puts(out_buf, max_out, &pos, "Error resolviendo DNS para curl.");
                return 1;
            }
            kprint("IP resuelta: "); kprint_ip(tip); kprint("\n");
            host_hdr = target;
        }

        kprint("HTTP GET a ");
        if (host_hdr) { kprint(host_hdr); kprint(" ("); kprint_ip(tip); kprint(")"); }
        else { kprint_ip(tip); }
        kprint(":"); kprint_dec(port); kprint(path); kprint("...\n");

        static char http_buf[4096];
        struct http_response resp;
        int code = http_get_host(tip, port, host_hdr, path, http_buf, sizeof(http_buf), &resp, 10000);
        if (code >= 0) {
            kprint("\n--- RESPUESTA HTTP [Status "); kprint_dec((uint32_t)code); kprint("] ---\n");
            if (resp.body && resp.body[0] != '\0') {
                kprint(resp.body);
            } else {
                /* Si no hay body (ej: 301 Moved Permanently), mostramos cabeceras recibidas */
                kprint(http_buf);
            }
            kprint("\n-------------------------------------\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, (resp.body && resp.body[0] != '\0') ? resp.body : http_buf);
        } else {
            kprint("Error en peticion HTTP (timeout o conexion cerrada).\n");
            if (http_buf[0] != '\0') {
                kprint("Datos parciales:\n");
                kprint(http_buf);
                kprint("\n");
            }
            uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Error en peticion HTTP.");
        }
        return 1;
    }

    if (cmd_line[0] == 't' && cmd_line[1] == 'e' && cmd_line[2] == 's' && cmd_line[3] == 't') {
        boot_gate_run_test_suite(out_buf, max_out);
        return 1;
    }

    if (src_tool(cmd_line, out_buf, max_out)) {
        return 1;
    }

    if (cmd_line[0] == 'l' && cmd_line[1] == 's' && (cmd_line[2] == '\0' || cmd_line[2] == ' ')) {
        vfs_list();
        vfs_format_list(out_buf, max_out);
        return 1;
    } else if (cmd_line[0] == 'c' && cmd_line[1] == 'a' && cmd_line[2] == 't' && cmd_line[3] == ' ') {
        const char *p = cmd_line + 4;
        while (*p == ' ') p++;
        char fn[48];
        uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        char buf[2048];
        int r = vfs_read(fn, buf, sizeof(buf));
        if (r >= 0) {
            kprint("\n"); kprint(buf); kprint("\n");
            uint32_t i = 0; while (buf[i] && i < max_out - 1) { out_buf[i] = buf[i]; i++; } out_buf[i] = '\0';
        } else {
            kprint("Error: archivo no encontrado: '"); kprint(fn); kprint("'\n");
            const char *err = "Error: archivo no encontrado en RamFS.";
            uint32_t i = 0; while (err[i] && i < max_out - 1) { out_buf[i] = err[i]; i++; } out_buf[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'w' && cmd_line[1] == 'r' && cmd_line[2] == 'i' && cmd_line[3] == 't' && cmd_line[4] == 'e' && cmd_line[5] == ' ') {
        const char *p = cmd_line + 6;
        while (*p == ' ') p++;
        char fn[48];
        uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        while (*p == ' ') p++;
        uint32_t tlen = 0; while (p[tlen]) tlen++;
        vfs_write(fn, p, tlen);
        kprint("Escrito y persistido en virtio-blk '"); kprint(fn); kprint("'\n");
        const char *succ = "Archivo escrito y persistido correctamente en disco virtio-blk.";
        uint32_t i = 0; while (succ[i] && i < max_fb_dummy - 1 && i < max_out - 1) { out_buf[i] = succ[i]; i++; } out_buf[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'r' && cmd_line[1] == 'm' && cmd_line[2] == ' ') {
        const char *fn = cmd_line + 3;
        while (*fn == ' ') fn++;
        if (vfs_delete(fn) == 0) {
            kprint("Eliminado '"); kprint(fn); kprint("'\n");
            const char *succ = "Archivo eliminado correctamente de RamFS.";
            uint32_t i = 0; while (succ[i] && i < max_out - 1) { out_buf[i] = succ[i]; i++; } out_buf[i] = '\0';
        } else {
            kprint("Error: no se pudo eliminar '"); kprint(fn); kprint("'\n");
            const char *err = "Error: el archivo no existe.";
            uint32_t i = 0; while (err[i] && i < max_out - 1) { out_buf[i] = err[i]; i++; } out_buf[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'm' && cmd_line[1] == 'e' && cmd_line[2] == 'm') {
        sysinfo_print_mem();
        sysinfo_format_mem(out_buf, max_out);
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 't' && cmd_line[2] == 'a' && cmd_line[3] == 't' && cmd_line[4] == 's') {
        sysinfo_print_stats();
        struct sysinfo s;
        sysinfo_get(&s);
        char tmp[256];
        uint32_t p = 0;
        const char *m = "VirtIO-NET: TX_pkts="; while (*m) tmp[p++] = *m++;
        uint32_t v = s.tx_packets; if (v==0) tmp[p++]='0'; else { char b[10]; int n=0; while(v){b[n++]='0'+(v%10);v/=10;} while(n) tmp[p++]=b[--n]; }
        m = " RX_pkts="; while (*m) tmp[p++] = *m++;
        v = s.rx_packets; if (v==0) tmp[p++]='0'; else { char b[10]; int n=0; while(v){b[n++]='0'+(v%10);v/=10;} while(n) tmp[p++]=b[--n]; }
        tmp[p] = '\0';
        for (uint32_t i = 0; i < p && i < max_out - 1; ++i) out_buf[i] = tmp[i];
        out_buf[p < max_out ? p : max_out - 1] = '\0';
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'c' && cmd_line[2] == 'i') {
        pci_scan();
        pci_format_scan(out_buf, max_out);
        return 1;
    } else if (cmd_line[0] == 'a' && cmd_line[1] == 'r' && cmd_line[2] == 'p') {
        arp_format_cache(out_buf, max_out);
        kprint("\n"); kprint(out_buf); kprint("\n");
        return 1;
    } else if (cmd_line[0] == 'h' && cmd_line[1] == 'e' && cmd_line[2] == 'a' && cmd_line[3] == 'p') {
        size_t used = 0, free_b = 0;
        kheap_stats(&used, &free_b);
        kprint("\nESTADO DEL HEAP (kmalloc):\n-------------------------\n");
        kprint("Base del Heap:    0x00400000 (4 MiB)\n");
        kprint("Alineacion:       16 bytes estricta\n");
        kprint("Memoria Usada:    "); kprint_dec((uint32_t)used); kprint(" bytes\n");
        kprint("Memoria Libre:    "); kprint_dec((uint32_t)(free_b / 1024)); kprint(" KiB\n");
        kprint("Capacidad total:  12 MiB\n\n");
        sysinfo_format_mem(out_buf, max_out);
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'i' && cmd_line[2] == 'n' && cmd_line[3] == 'g') {
        const char *arg = cmd_line + 4;
        while (*arg == ' ') arg++;
        uint8_t target[4];
        if (*arg) parse_ip(arg, target);
        else { target[0] = net_gateway[0]; target[1] = net_gateway[1]; target[2] = net_gateway[2]; target[3] = net_gateway[3]; }
        kprint("PING a "); kprint_ip(target); kprint("...\n");
        int r = icmp_ping(target, 1, 2000);
        const char *resp = (r == 1) ? "Ping exitoso: respuesta ICMP recibida." : "Ping fallido: tiempo de espera agotado.";
        uint32_t i = 0; while (resp[i] && i < max_out - 1) { out_buf[i] = resp[i]; i++; } out_buf[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'r') {
        const char *p = cmd_line + 11;
        uint64_t sec = parse_num(&p);
        char sbuf[512];
        if (virtio_blk_read(sec, sbuf) == 0) {
            kprint("\n[SECTOR LBA "); kprint_dec((uint32_t)sec); kprint("]:\n");
            for (int i = 0; i < 512 && sbuf[i]; i++) {
                if (sbuf[i] >= 32 && sbuf[i] < 127) kputc(sbuf[i]);
            }
            kprint("\n");
            uint32_t i = 0; while (sbuf[i] && i < max_out - 1 && i < 511) { out_buf[i] = sbuf[i]; i++; } out_buf[i] = '\0';
        } else {
            const char *err = "Error de I/O al leer el disco duro.";
            uint32_t i = 0; while (err[i] && i < max_out - 1) { out_buf[i] = err[i]; i++; } out_buf[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'w') {
        const char *p = cmd_line + 12;
        uint64_t sec = parse_num(&p);
        if (sec < SECTOR_USER_MIN) {
            kprint("Error: sectores 0-2047 reservados al sistema y RamFS persistente.\n");
            const char *err = "Error: sector protegido (LBA < 2048 reservado). Usa LBA >= 2048.";
            uint32_t i = 0; while (err[i] && i < max_out - 1) { out_buf[i] = err[i]; i++; } out_buf[i] = '\0';
            return 1;
        }
        while (*p == ' ') p++;
        char sbuf[512];
        memset(sbuf, 0, sizeof(sbuf));
        int bi = 0;
        while (*p && bi < 511) sbuf[bi++] = *p++;
        if (virtio_blk_write(sec, sbuf) == 0) {
            kprint("Sector LBA escrito en disco persistente.\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, "Sector LBA ");
            fb_put_dec(out_buf, max_out, &pos, (uint32_t)sec);
            fb_puts(out_buf, max_out, &pos, " escrito con exito (");
            fb_put_dec(out_buf, max_out, &pos, (uint32_t)bi);
            fb_puts(out_buf, max_out, &pos, " B datos).");
        } else {
            const char *err = "Error de I/O al escribir en disco duro.";
            uint32_t i = 0; while (err[i] && i < max_out - 1) { out_buf[i] = err[i]; i++; } out_buf[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'f' && cmd_line[1] == 's' && cmd_line[2] == '-' && cmd_line[3] == 's' && cmd_line[4] == 'y' && cmd_line[5] == 'n' && cmd_line[6] == 'c') {
        vfs_sync();
        kprint("RamFS sincronizado con exito en virtio-blk (LBA 1024).\n");
        const char *succ = "RamFS sincronizado en disco persistente.";
        uint32_t i = 0; while (succ[i] && i < max_out - 1) { out_buf[i] = succ[i]; i++; } out_buf[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'f' && cmd_line[1] == 's' && cmd_line[2] == '-' && cmd_line[3] == 'f' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == 'm' && cmd_line[7] == 'a' && cmd_line[8] == 't') {
        vfs_format();
        kprint("RamFS formateado a estado de fabrica en virtio-blk.\n");
        const char *succ = "RamFS formateado a fabrica.";
        uint32_t i = 0; while (succ[i] && i < max_out - 1) { out_buf[i] = succ[i]; i++; } out_buf[i] = '\0';
        return 1;
    }

    return 0;
}

/* Ejecuta la herramienta y captura un resumen de salida para retroalimentar a la IA */
static int execute_tool_with_feedback(const char *cmd_line, char *feedback_out, uint32_t max_fb)
{
    if (dispatch_command(cmd_line, feedback_out, max_fb)) {
        return 1;
    }
    const char *unrec = "Comando ejecutado.";
    uint32_t i = 0; while (unrec[i] && i < max_fb - 1) { feedback_out[i] = unrec[i]; i++; } feedback_out[i] = '\0';
    return 0;
}

static int is_valid_tool(const char *s)
{
    /* Rechazar categoricamente cualquier orden que contenga marcadores '<' o '>' de plantilla */
    for (const char *chk = s; *chk && *chk != '\n' && *chk != '\r'; chk++) {
        if (*chk == '<' || *chk == '>') return 0;
    }

    if (s[0] == 's' && s[1] == 't' && s[2] == 'a' && s[3] == 't' && s[4] == 's') return 1;
    if (s[0] == 'm' && s[1] == 'e' && s[2] == 'm') return 1;
    if (s[0] == 'a' && s[1] == 'r' && s[2] == 'p') return 1;
    if (s[0] == 'p' && s[1] == 'c' && s[2] == 'i') return 1;
    if (s[0] == 'p' && s[1] == 'i' && s[2] == 'n' && s[3] == 'g') return 1;
    if (s[0] == 'l' && s[1] == 's') return 1;
    if (s[0] == 'c' && s[1] == 'a' && s[2] == 't' && s[3] == ' ') return 1;
    if (s[0] == 'w' && s[1] == 'r' && s[2] == 'i' && s[3] == 't' && s[4] == 'e' && s[5] == ' ') return 1;
    if (s[0] == 'r' && s[1] == 'm' && s[2] == ' ') return 1;
    if (s[0] == 'h' && s[1] == 'o' && s[2] == 's' && s[3] == 't' && s[4] == '_' && s[5] == 'p') return 1;
    if (s[0] == 's' && s[1] == 'e' && s[2] == 'c' && s[3] == 't' && s[4] == 'o' && s[5] == 'r' && s[6] == '_') return 1;
    if (s[0] == 's' && s[1] == 'r' && s[2] == 'c' && s[3] == '_') return 1;
    if (s[0] == 't' && s[1] == 'e' && s[2] == 's' && s[3] == 't') return 1;
    if (s[0] == 'u' && s[1] == 'p' && s[2] == 't' && s[3] == 'i' && s[4] == 'm' && s[5] == 'e') return 1;
    if (s[0] == 's' && s[1] == 'l' && s[2] == 'e' && s[3] == 'e' && s[4] == 'p' && s[5] == ' ') return 1;
    if (s[0] == 'd' && s[1] == 'n' && s[2] == 's' && s[3] == ' ') return 1;
    if (s[0] == 'c' && s[1] == 'u' && s[2] == 'r' && s[3] == 'l' && s[4] == ' ') return 1;

    return 0;
}


/* ---- Puente host (PATCHv02) --------------------------------------
 * Buzon en LBA 1..16 (8 KiB):
 *   [0..7]  "PATCHv02"
 *   [8..11] longitud del texto (u32 little-endian)
 *   [12..]  texto: bloques FILE / SEARCH / REPLACE
 * Es static: 8 KiB en la pila (16 KiB) seria peligroso.
 */
#define MAILBOX_SIZE  8192
#define MAILBOX_LBA   1
#define MAILBOX_HDR   12

static char mailbox_buf[MAILBOX_SIZE];

static void host_submit_patch(const char *text)
{
    uint32_t n = 0;
    while (text[n]) n++;

    if (n == 0 || n > MAILBOX_SIZE - MAILBOX_HDR) {
        kprint("\n[PARCHE] Rechazado: tamano invalido (");
        kprint_dec(n);
        kprint(" bytes, maximo 8180).\n");
        return;
    }

    memset(mailbox_buf, 0, sizeof(mailbox_buf));
    memcpy(mailbox_buf, "PATCHv02", 8);
    memcpy(mailbox_buf + 8, &n, 4);              /* x86: little-endian */
    memcpy(mailbox_buf + MAILBOX_HDR, text, n);

    for (int s = 0; s < MAILBOX_SIZE / 512; ++s) {
        if (virtio_blk_write(MAILBOX_LBA + s, mailbox_buf + s * 512) != 0) {
            kprint("\n[PARCHE] Error de I/O escribiendo el buzon.\n");
            return;
        }
    }

    kprint("\n========================================================================\n");
    kprint("!!! PUENTE HOST-BRIDGE: PARCHE ENVIADO, SOLICITANDO RECOMPILACION !!!\n");
    kprint("========================================================================\n");

    qemu_exit(0x10);                             /* QEMU sale con codigo 0x21 */

    /* Fallback si no existe isa-debug-exit: triple fault (con -no-reboot cierra QEMU). */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = {0, 0};
    __asm__ volatile ("lidt %0; int3" :: "m"(idtr));
}

/* ---- hpatch: prueba del puente host sin LLM ----------------------
 *   hpatch ok          anade un comentario inocuo a src/console.h
 *   hpatch bad         introduce un #error -> el host debe hacer rollback
 *   hpatch raw <texto> parche literal; "\n" se convierte en salto de linea
 * Tras reiniciar, 'sector_read 20' muestra el resultado del host.
 */
static char hpatch_buf[2048];

static int hp_starts(const char *s, const char *w)
{
    while (*w) {
        if (*s != *w) return 0;
        s++;
        w++;
    }

    return *s == '\0' || *s == ' ';
}

static void hpatch_command(const char *arg)
{
    while (*arg == ' ') arg++;

    if (hp_starts(arg, "ok")) {
        host_submit_patch("FILE: src/console.h\n<<<<<<< SEARCH\n#define MYOS_CONSOLE_H\n=======\n#define MYOS_CONSOLE_H\n/* hpatch ok */\n>>>>>>> REPLACE\n");
    } else if (hp_starts(arg, "bad")) {
        host_submit_patch("FILE: src/console.h\n<<<<<<< SEARCH\n#define MYOS_CONSOLE_H\n=======\n#define MYOS_CONSOLE_H\n#error hpatch_bad_test\n>>>>>>> REPLACE\n");
    } else if (hp_starts(arg, "bootfail")) {
        host_submit_patch("FILE: src/llm.c\n<<<<<<< SEARCH\nint llm_health(void)\n{\n=======\nint llm_health(void)\n{\n    return 0; /* Simular fallo de enlace en canary boot */\n>>>>>>> REPLACE\n");
    } else if (hp_starts(arg, "raw")) {
        arg += 3;
        while (*arg == ' ') arg++;

        uint32_t n = 0;

        while (*arg && n < sizeof(hpatch_buf) - 1) {
            if (arg[0] == '\\' && arg[1] == 'n') {
                hpatch_buf[n++] = '\n';
                arg += 2;
            } else {
                hpatch_buf[n++] = *arg++;
            }
        }

        hpatch_buf[n] = '\0';
        host_submit_patch(hpatch_buf);
    } else {
        kprint("Uso: hpatch ok | hpatch bad | hpatch raw <texto con \\n>\n");
    }
}

/* ---- Persistencia de la mision del agente ---------------------------
 * Si el agente envia un parche, el kernel se reinicia. La mision y el
 * historial se guardan en el RamFS persistente (/agent/state) y, tras el
 * reinicio, el host arma la bandera LBA 25 para que se reanude sola.
 * Formato:  ATTEMPTS:<n>\n<mision>\n@@HISTORY@@\n<historial>
 */
#define RESULT_LBA          20
#define RESUME_FLAG_LBA     25
#define AGENT_STATE_FILE    "/agent/state"
#define AGENT_MAX_PATCHES   3
#define AGENT_HIST_KEEP     1800
#define AGENT_HIST_MARK     "\n@@HISTORY@@\n"

static char history_buf[4096];
static char agent_state_buf[4096];
static char agent_resume_mission[512];
static int  agent_resume_pending = 0;   /* 1 = hay una mision que reanudar */
static int  agent_patch_attempts = 0;

static void agent_state_save(const char *mission, const char *history, uint32_t hist_len)
{
    static uint8_t zero_sec[512];       /* BSS: a cero */
    uint32_t pos = 0;
    uint32_t start = hist_len > AGENT_HIST_KEEP ? hist_len - AGENT_HIST_KEEP : 0;

    if (start > 0) {
        const char *nx = find_substr(history + start, "\n- Paso ");
        start = nx ? (uint32_t)(nx + 1 - history) : hist_len;
    }

    agent_state_buf[0] = '\0';
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "ATTEMPTS:");
    fb_put_dec(agent_state_buf, sizeof(agent_state_buf), &pos, (uint32_t)(agent_patch_attempts + 1));
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "\n");
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, mission);
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, AGENT_HIST_MARK);
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, history + start);

    vfs_write(AGENT_STATE_FILE, agent_state_buf, pos);

    /* El resultado que se lea al volver sera el de ESTE parche. */
    virtio_blk_write(RESULT_LBA, zero_sec);
}

static void agent_state_load(void)
{
    static uint8_t sec[512];
    static uint8_t res[512];

    if (virtio_blk_read(RESUME_FLAG_LBA, sec) != 0 || memcmp(sec, "RESUME_REQ", 10) != 0) {
        return;
    }

    memset(sec, 0, sizeof(sec));
    virtio_blk_write(RESUME_FLAG_LBA, sec);      /* una reanudacion por orden del host */

    if (vfs_read(AGENT_STATE_FILE, agent_state_buf, sizeof(agent_state_buf)) <= 0) {
        return;
    }
    vfs_delete(AGENT_STATE_FILE);

    const char *p = agent_state_buf;

    if (memcmp(p, "ATTEMPTS:", 9) != 0) {
        return;
    }
    p += 9;

    int n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
    }
    if (*p == '\n') p++;

    const char *hm = find_substr(p, AGENT_HIST_MARK);
    if (!hm) {
        return;
    }

    uint32_t m = 0;
    while (p + m < hm && m < sizeof(agent_resume_mission) - 1) {
        agent_resume_mission[m] = p[m];
        m++;
    }
    agent_resume_mission[m] = '\0';
    if (m == 0) {
        return;
    }

    uint32_t pos = 0;
    history_buf[0] = '\0';
    fb_puts(history_buf, sizeof(history_buf), &pos, hm + (sizeof(AGENT_HIST_MARK) - 1));
    if (pos > 0 && history_buf[pos - 1] != '\n') {
        fb_putc(history_buf, sizeof(history_buf), &pos, '\n');
    }

    if (virtio_blk_read(RESULT_LBA, res) == 0 && res[0] != 0) {
        res[511] = 0;
        for (int i = 0; res[i]; ++i) {
            if (res[i] < 32 || res[i] > 126) res[i] = ' ';
        }
    } else {
        res[0] = '\0';
    }

    if (find_substr((const char *)res, "BOOT_OK")) {
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "- Paso R (Reinicio): El host APROBO y COMPILO tu parche con exito (BOOT_OK).\n"
                "  [DIRECTIVA]: Tu parche ya esta aplicado en el kernel. NO vuelvas a enviar un parche.\n"
                "  Verifica el resultado (con src_grep o src_cat) y concluye la mision emitiendo action=\"final\" con tu verdict.\n");
    } else if (find_substr((const char *)res, "FAILED") || find_substr((const char *)res, "REJECTED")) {
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "- Paso R (Reinicio): El parche NO fue aceptado o fallo la compilacion/arranque (rollback aplicado).\n"
                "  [DETALLE DEL HOST]: ");
        fb_puts(history_buf, sizeof(history_buf), &pos, (const char *)res);
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "\n  [DIRECTIVA]: Analiza el error anterior, corrige tu SEARCH/REPLACE y vuelve a intentarlo.\n");
    } else {
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "- Paso R (Reinicio): El kernel se reinicio tras el parche. Resultado: ");
        fb_puts(history_buf, sizeof(history_buf), &pos, res[0] ? (const char *)res : "(sin resultado del host)");
        fb_puts(history_buf, sizeof(history_buf), &pos, "\n");
    }

    agent_patch_attempts = n;
    agent_resume_pending = 1;
}

/* ---- Ayudas del agente: parche estructurado y notas de formato ------ */

/* Lee una clave en minusculas o, si no esta, en MAYUSCULAS. */
static int json_get2(const char *json, const char *k1, const char *k2, char *out, uint32_t max)
{
    int n = llm_json_get(json, k1, out, max);

    if (n <= 0) {
        n = llm_json_get(json, k2, out, max);
    }

    return n;
}

/* Anade "- Paso N: <msg>" al historial para que el modelo no repita el fallo. */
static void history_add_note(uint32_t *hist_len, int step, const char *msg)
{
    uint32_t pos = *hist_len;

    if (pos + 400 > sizeof(history_buf)) {
        return;
    }

    fb_puts(history_buf, sizeof(history_buf), &pos, "- Paso ");
    fb_putc(history_buf, sizeof(history_buf), &pos, (char)('0' + step));
    fb_puts(history_buf, sizeof(history_buf), &pos, ": ");
    fb_puts(history_buf, sizeof(history_buf), &pos, msg);
    fb_puts(history_buf, sizeof(history_buf), &pos, "\n");

    *hist_len = pos;
}

/* Buffers de agente en BSS para blindar la pila */
static char agent_prompt_buf[8192];
static char agent_reply_buf[4096];
static char tool_feedback_buf[2048];

/* ---- Subsistema de Memoria Persistente Categorizada ------------- */
static void inject_agent_memory(char *dst, uint32_t max, uint32_t *pos)
{
    static const char *const mem_files[] = {
        "/etc/mem_user.txt",
        "/etc/mem_hw.txt",
        "/etc/mem_kernel.txt"
    };
    static const char *const mem_labels[] = {
        "- Usuario/Identidad: ",
        "- Hardware/Red: ",
        "- Codigo/Kernel: "
    };

    int any = 0;
    char buf[384];

    for (int i = 0; i < 3; ++i) {
        int n = vfs_read(mem_files[i], buf, sizeof(buf));
        if (n > 0) {
            if (!any) {
                fb_puts(dst, max, pos, "\n[MEMORIA PERMANENTE APRENDIDA EN MISIONES ANTERIORES]:\n");
                any = 1;
            }
            fb_puts(dst, max, pos, mem_labels[i]);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                buf[--n] = '\0';
            }
            fb_puts(dst, max, pos, buf);
            fb_puts(dst, max, pos, "\n");
        }
    }
}

static void shell_run(void)
{
    char cmd[512];
    char reply[4096];

    serial_print("\n============================================================\n");
    serial_print("MYOS 0.1 INTERACTIVE SHELL\n");
    serial_print("Escribe 'help' para ver comandos disponibles.\n");
    serial_print("============================================================\n\n");

    for (;;) {
        int len;

        if (agent_resume_pending == 1) {
            /* Reanudar la mision tras un reinicio por parche. */
            const char *pre = "agent ";
            uint32_t ci = 0;
            while (*pre) cmd[ci++] = *pre++;
            for (uint32_t k = 0; agent_resume_mission[k] && ci < sizeof(cmd) - 1; ++k) {
                cmd[ci++] = agent_resume_mission[k];
            }
            cmd[ci] = '\0';
            len = (int)ci;
            serial_print("myos> ");
            serial_print(cmd);
            serial_print("   [reanudacion automatica tras el reinicio]\n");
        } else {
            serial_print("myos> ");
            len = kgetline(cmd, sizeof(cmd));
        }
        if (len == 0) {
            continue;
        }

        {   /* ignorar espacios iniciales (p. ej. al pegar comandos) */
            char *s0 = cmd;
            while (*s0 == ' ') s0++;
            if (s0 != cmd) {
                uint32_t k = 0;
                while (s0[k]) { cmd[k] = s0[k]; k++; }
                cmd[k] = '\0';
                len = (int)k;
            }
            if (len == 0) continue;
        }
        static char shell_buf[2048];
        if (dispatch_command(cmd, shell_buf, sizeof(shell_buf))) {
            continue;
        }

        if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && (cmd[4] == '\0' || cmd[4] == ' ')) {
            serial_print("Comandos disponibles:\n");
            serial_print("  help                 - Muestra esta ayuda\n");
            serial_print("  creador              - Muestra informacion del creador de MYOS\n");
            serial_print("  test_suite           - Ejecuta la suite de auto-test y no-regresion\n");
            serial_print("  uptime               - Tiempo de ejecucion del kernel\n");
            serial_print("  sleep <ms>           - Suspende la CPU con HLT durante N milisegundos\n");
            serial_print("  dns <dominio>        - Consulta de registro A en servidor DNS\n");
            serial_print("  curl <host|ip> [pt]  - Peticion HTTP GET con resolucion DNS y cabecera Host\n");
            serial_print("  health               - Verifica estado del servidor LLM\n");
            serial_print("  llm <mensaje>        - Consulta general a nail-35b\n");
            serial_print("  llm-diag [pregunta]  - Telemetria + Diagnostico del kernel por IA\n");
            serial_print("  agent <mision>       - Agente autonomo con ejecucion de herramientas\n");
            serial_print("  heap                 - Estado de la memoria dinamica kmalloc\n");
            serial_print("  ping <ip>            - Envia ICMP echo a una direccion IPv4\n");
            serial_print("  ls                   - Lista los archivos del RamFS\n");
            serial_print("  cat <archivo>        - Muestra el contenido de un archivo\n");
            serial_print("  write <arch> <texto> - Crea o sobrescribe un archivo\n");
            serial_print("  fs-sync              - Fuerza sincronizacion de RamFS a virtio-blk\n");
            serial_print("  fs-format            - Restaura RamFS al estado inicial de fabrica\n");
            serial_print("  rm <archivo>         - Elimina un archivo\n");
            serial_print("  mem                  - Informacion de CPU, paginacion y memoria\n");
            serial_print("  stats                - Estadisticas de trafico VirtIO-NET\n");
            serial_print("  arp                  - Muestra la tabla de cache ARP\n");
            serial_print("  pci                  - Escanea los dispositivos PCI\n");
            serial_print("  status               - Muestra estado de red e IP\n");
            serial_print("  clear                - Limpia la pantalla VGA\n");
            serial_print("  src_ls               - Lista los fuentes del kernel (leidos del disco)\n");
            serial_print("  src_cat <f> [off]    - Muestra un fragmento (768 B) de un fuente\n");
            serial_print("  src_grep <f> <txt>   - Busca lineas en un fuente (devuelve offsets)\n");
            serial_print("  sector_read <lba>    - Lee un sector (LBA 20 = resultado del ultimo parche)\n");
            serial_print("  hpatch ok|bad|bootfail|raw - Prueba del puente y la puerta de arranque\n");
            serial_print("  panic [pf|div|ud]    - Prueba el gestor de excepciones de la IDT\n");
        } else if (cmd[0] == 'l' && cmd[1] == 'l' && cmd[2] == 'm' && cmd[3] == '-' && cmd[4] == 'd' && cmd[5] == 'i' && cmd[6] == 'a' && cmd[7] == 'g') {
            const char *q = cmd + 8;
            while (*q == ' ') q++;
            serial_print("Recopilando telemetria y solicitando diagnostico a nail-35b...\n");
            if (llm_diagnose(q, reply, sizeof(reply), 35000)) {
                serial_print("\n[DIAGNOSTICO IA nail-35b]:\n");
                serial_print(reply);
                serial_print("\n\n");
            } else {
                serial_print("Error al solicitar diagnostico al LLM.\n");
            }
} else if (cmd[0] == 'a' && cmd[1] == 'g' && cmd[2] == 'e' && cmd[3] == 'n' && cmd[4] == 't' && cmd[5] == ' ') {
            const char *mission = cmd + 6;
            while (*mission == ' ') mission++;
            if (*mission == '\0') {
                serial_print("Uso: agent <mision en lenguaje natural>\n");
                continue;
            }

            serial_print("\n[AGENTE AUTONOMO]: Iniciando mision multi-paso...\n");

            /* Paso inicial: bucle ReAct multi-paso */

            /* Bucle ReAct con Salida Estructurada JSON y Grammar */
            uint32_t hist_len = 0;
            if (agent_resume_pending == 1) {
                /* historial recuperado por agent_state_load() */
                agent_resume_pending = 0;
                while (history_buf[hist_len]) hist_len++;
            } else {
                history_buf[0] = '\0';
            }

            uint32_t ap_len = 0;
            int finished = 0;
            for (int step = 1; step <= 8; ++step) {
                ap_len = 0;
                const char *p_m = "MISION: '"; while (*p_m) agent_prompt_buf[ap_len++] = *p_m++;
                const char *p_mval = mission; while (*p_mval && ap_len < sizeof(agent_prompt_buf) - 1200) agent_prompt_buf[ap_len++] = *p_mval++;
                const char *p_ctx = "'.\nContexto MYOS: Kernel bare-metal x86_64. Red: IP local 10.0.2.15, Gateway 10.0.2.2. RamFS en /.\n"
                                    "Responde SIEMPRE con un objeto JSON valido con este esquema exacto:\n"
                                    "{\n"
                                    "  \"thought\": \"analisis breve de la accion a tomar\",\n"
                                    "  \"action\": \"tool\" | \"patch\" | \"final\",\n"
                                    "  \"cmd\": \"herramienta a ejecutar si action==tool\",\n"
                                    "  \"patch\": {\"file\": \"src/archivo.c\", \"search\": \"texto exacto existente, una sola vez\", \"replace\": \"texto nuevo\"} (con action==patch; cadenas vacias en otro caso),\n"
                                    "  \"verdict\": \"resumen completo y detallado para el usuario si action==final\"\n"
                                    "}\n"
                                    "Herramientas validas:\n"
                                    "- Diagnostico/Sistema: stats | mem | arp | pci | ping 10.0.2.2 | uptime | sleep MS | test_suite\n"
                                    "- Red/Internet: dns DOMINIO | curl HOST [PUERTO] [RUTA]\n"
                                    "- Archivos/Memoria: ls | cat /archivo | write /archivo texto | rm /archivo\n"
                                    "- Disco/Sectores: sector_read LBA | sector_write LBA texto\n"
                                    "- Codigo Fuente: src_ls | src_cat archivo.c OFFSET | src_grep archivo.c texto\n"
                                    "Reglas de oro:\n"
                                    "1) Si la mision pide varias tareas (ej: resolver DNS y hacer curl), ejecuta UNA herramienta por paso hasta completar TODAS.\n"
                                    "2) Para verificar el estado general del kernel ejecuta 'test_suite'.\n"
                                    "3) En 'verdict' explica con claridad y detalle todo lo realizado. NUNCA uses respuestas vacias ni '...'.\n"
                                    "4) Para modificar el codigo: localiza con src_grep, lee con src_cat y copia el SEARCH EXACTO (debe aparecer una sola vez). Con action==patch el kernel se reiniciara y recibiras el resultado del host.\n"
                                    "5) Si el historial indica que tu parche fue aprobado (BOOT_OK), NO envies mas parches: comprueba el archivo y concluye con action=\"final\".\n"
                                    "6) MEMORIA A LARGO PLAZO: Para recordar aprendizajes permanentes entre misiones, escribe de forma concisa con 'write' en /etc/mem_user.txt (usuario/identidad), /etc/mem_hw.txt (red/hardware) o /etc/mem_kernel.txt (codigo).\n";
                while (*p_ctx) agent_prompt_buf[ap_len++] = *p_ctx++;

                /* Inyectar recuerdos persistentes aprendidos previamente */
                inject_agent_memory(agent_prompt_buf, sizeof(agent_prompt_buf), &ap_len);

                if (hist_len > 0) {
                    const char *h_hdr = "Acciones ya realizadas anteriormente:\n";
                    while (*h_hdr) agent_prompt_buf[ap_len++] = *h_hdr++;
                    for (uint32_t h = 0; h < hist_len && ap_len < sizeof(agent_prompt_buf) - 400; ++h) {
                        agent_prompt_buf[ap_len++] = history_buf[h];
                    }
                }

                const char *p_rules = "\nInstruccion: Analiza el historial, determina que falta para completar la mision y genera el JSON.";
                while (*p_rules && ap_len < sizeof(agent_prompt_buf) - 1) agent_prompt_buf[ap_len++] = *p_rules++;
                agent_prompt_buf[ap_len] = '\0';

                if (!llm_chat_json(agent_prompt_buf, agent_reply_buf, sizeof(agent_reply_buf), 40000)) {
                    serial_print("Error en la comunicacion estructurada con el agente.\n");
                    finished = 1;
                    break;
                }

                static char act[32];
                static char th[512];
                static char cmd_field[512];
                static char patch_field[4096];
                static char verdict[4096];

                act[0] = '\0'; th[0] = '\0'; cmd_field[0] = '\0'; patch_field[0] = '\0'; verdict[0] = '\0';

                llm_json_get(agent_reply_buf, "action", act, sizeof(act));
                llm_json_get(agent_reply_buf, "thought", th, sizeof(th));
                llm_json_get(agent_reply_buf, "cmd", cmd_field, sizeof(cmd_field));
                llm_json_get(agent_reply_buf, "patch", patch_field, sizeof(patch_field));
                llm_json_get(agent_reply_buf, "verdict", verdict, sizeof(verdict));

                if (th[0]) {
                    serial_print("\n>> [PASO ");
                    serial_put_dec((uint8_t)step);
                    serial_print(" | ANALISIS]: ");
                    serial_print(th);
                    serial_print("\n");
                }

                /* Parche estructurado: patch = {file, search, replace} -> bloque FILE/SEARCH/REPLACE */
                if (find_substr(act, "patch") && patch_field[0] == '\0') {
                    static char pf[96];
                    static char ps[2048];
                    static char pr[2048];

                    pf[0] = '\0'; ps[0] = '\0'; pr[0] = '\0';
                    json_get2(agent_reply_buf, "file", "FILE", pf, sizeof(pf));
                    json_get2(agent_reply_buf, "search", "SEARCH", ps, sizeof(ps));
                    json_get2(agent_reply_buf, "replace", "REPLACE", pr, sizeof(pr));

                    if (pf[0] && ps[0]) {
                        const char *pfp = pf;
                        uint32_t pp = 0;

                        while (*pfp == '/') pfp++;

                        fb_puts(patch_field, sizeof(patch_field), &pp, "FILE: ");
                        if (!(pfp[0] == 's' && pfp[1] == 'r' && pfp[2] == 'c' && pfp[3] == '/')) {
                            fb_puts(patch_field, sizeof(patch_field), &pp, "src/");
                        }
                        fb_puts(patch_field, sizeof(patch_field), &pp, pfp);
                        fb_puts(patch_field, sizeof(patch_field), &pp, "\n<<<<<<< SEARCH\n");
                        fb_puts(patch_field, sizeof(patch_field), &pp, ps);
                        fb_puts(patch_field, sizeof(patch_field), &pp, "\n=======\n");
                        fb_puts(patch_field, sizeof(patch_field), &pp, pr);
                        fb_puts(patch_field, sizeof(patch_field), &pp, "\n>>>>>>> REPLACE\n");
                    }
                }

                /* 1. Accion: Parche al host */
                if (find_substr(act, "patch") || patch_field[0] != '\0') {
                    const char *ptext = patch_field[0] ? patch_field : agent_reply_buf;
                    const char *pfile = find_substr(ptext, "FILE: src/");
                    if (pfile) {
                        serial_print("\n[AGENTE AUTONOMO]: Parche estructurado emitido. Enviando al host...\n");
                        if (agent_patch_attempts >= AGENT_MAX_PATCHES) {
                            serial_print("\n[AGENTE AUTONOMO]: limite de parches por mision alcanzado; no se envia.\n");
                            finished = 1;
                            break;
                        }
if (hist_len + 400 < sizeof(history_buf)) {
                            fb_puts(history_buf, sizeof(history_buf), &hist_len, "- Paso ");
                            fb_putc(history_buf, sizeof(history_buf), &hist_len, (char)('0' + step));
                            fb_puts(history_buf, sizeof(history_buf), &hist_len, ": emitiste un parche estructurado para modificar el codigo.\n");
                        }
                        agent_state_save(mission, history_buf, hist_len);
                        host_submit_patch(pfile);
                        /* Solo se llega aqui si el envio fallo: descartar el estado. */
                        vfs_delete(AGENT_STATE_FILE);
                        finished = 1;
                        break;
                    }
                }

                /* 2. Accion: Herramienta de hardware */
                if (find_substr(act, "tool") || (cmd_field[0] != '\0' && !find_substr(act, "final"))) {
                    const char *clean_cmd = cmd_field;
                    while (*clean_cmd == ' ') clean_cmd++;

                    if (is_valid_tool(clean_cmd)) {
                        serial_print(">> [ACCION IA]: Ejecutando '");
                        serial_print(clean_cmd);
                        serial_print("' en el hardware...\n\n");

                        execute_tool_with_feedback(clean_cmd, tool_feedback_buf, sizeof(tool_feedback_buf));

                        serial_print("\n>> [FEEDBACK A LA IA]: ");
                        serial_print(tool_feedback_buf);
                        serial_print("\n");

                        /* Recorte de historial */
                        while (hist_len + 1300 > sizeof(history_buf)) {
                            const char *nx = find_substr(history_buf + 1, "\n- Paso ");
                            if (!nx) { hist_len = 0; history_buf[0] = '\0'; break; }
                            uint32_t cut = (uint32_t)(nx - history_buf) + 1;
                            memmove(history_buf, history_buf + cut, hist_len - cut + 1);
                            hist_len -= cut;
                        }

                        const char *a1 = "- Paso "; while (*a1 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a1++;
                        history_buf[hist_len++] = (char)('0' + step);
                        const char *a2 = ": ejecutaste '"; while (*a2 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a2++;
                        const char *a3 = clean_cmd; while (*a3 && hist_len < sizeof(history_buf) - 150) history_buf[hist_len++] = *a3++;
                        const char *a4 = "' -> Resultado: '"; while (*a4 && hist_len < sizeof(history_buf) - 100) history_buf[hist_len++] = *a4++;
                        const char *a5 = tool_feedback_buf; while (*a5 && hist_len < sizeof(history_buf) - 20) history_buf[hist_len++] = *a5++;
                        const char *a6 = "'\n"; while (*a6 && hist_len < sizeof(history_buf) - 1) history_buf[hist_len++] = *a6++;
                        history_buf[hist_len] = '\0';
                        continue;
                    }
                }

                /* 3. Accion: Dictamen final o aviso de respuesta incompleta */
                if (find_substr(act, "final") || verdict[0] != '\0') {
                    serial_print("\n[AGENTE DICTAMEN FINAL]:\n");
                    serial_print(verdict[0] ? verdict : agent_reply_buf);
                    serial_print("\n\n");
                    finished = 1;
                    break;
                }

                serial_print("\n[AVISO IA]: Respuesta incompleta o no estructurada recibida del modelo:\n");
                serial_print(agent_reply_buf);
                history_add_note(&hist_len, step,
                    "tu respuesta fue rechazada por formato: usa action=tool con cmd, "
                    "action=patch con patch={file,search,replace} (cadenas no vacias) "
                    "o action=final con verdict.");
                serial_print("\nReintentando paso...\n");
            }            if (!finished) {
                serial_print("\nMision concluida.\n\n");
            }
        } else if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'a' && cmd[3] == 'l' && cmd[4] == 't' && cmd[5] == 'h') {
            serial_print("Comprobando /health en servidor LLM...\n");
            if (llm_health()) {
                serial_print("Servidor LLM: OK (HTTP 200 OK)\n");
            } else {
                serial_print("Servidor LLM: ERROR\n");
            }
        } else if (cmd[0] == 'l' && cmd[1] == 'l' && cmd[2] == 'm' && cmd[3] == ' ') {
            const char *prompt = cmd + 4;
            while (*prompt == ' ') prompt++;
            if (*prompt == '\0') {
                serial_print("Uso: llm <mensaje>\n");
                continue;
            }
            serial_print("Consultando a nail-35b...\n");
            if (llm_chat(prompt, reply, sizeof(reply), 30000)) {
                serial_print("\n[nail-35b]: ");
                serial_print(reply);
                serial_print("\n\n");
            } else {
                serial_print("Error al consultar el LLM.\n");
            }
        } else if (cmd[0] == 's' && cmd[1] == 't' && cmd[2] == 'a' && cmd[3] == 't' && cmd[4] == 'u' && cmd[5] == 's') {
            serial_print("IP:  "); kprint_ip(net_ip);
            serial_print("  GW: "); kprint_ip(net_gateway);
            serial_print("  MAC: "); kprint_mac(net_mac);
            serial_print("\n");
        } else if (cmd[0] == 'h' && cmd[1] == 'p' && cmd[2] == 'a' && cmd[3] == 't' && cmd[4] == 'c' && cmd[5] == 'h') {
            hpatch_command(cmd + 6);
                } else if (cmd[0] == 'p' && cmd[1] == 'a' && cmd[2] == 'n' && cmd[3] == 'i' && cmd[4] == 'c') {
            const char *arg = cmd + 5;
            while (*arg == ' ') arg++;
            if (arg[0] == 'd' && arg[1] == 'i' && arg[2] == 'v') {
                serial_print("Provocando division por cero (#DE)...\n");
                volatile int zero = 0;
                volatile int x = 42 / zero;
                (void)x;
            } else if (arg[0] == 'u' && arg[1] == 'd') {
                serial_print("Ejecutando instruccion invalida (#UD)...\n");
                __asm__ volatile ("ud2");
            } else {
                serial_print("Provocando fallo de pagina (#PF) en 0xDEADBEEF00...\n");
                volatile uint64_t *bad_ptr = (volatile uint64_t *)0xDEADBEEF00ULL;
                *bad_ptr = 0xCAFEBABE;
            }
} else if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r') {
            clear_screen();
        } else {
            serial_print("Comando desconocido: '");
            serial_print(cmd);
            serial_print("'. Escribe 'help'.\n");
        }
    }
}


void kernel_main(void)
{
    serial_init();
    idt_init();
    timer_calibrate_tsc();
    timer_init(1000); /* PIT IRQ0 a 1000 Hz (1 ms tick) */
    kprint("PIT: IRQ0 activo a 1000 Hz (ticks de 1 ms)\n");
    __asm__ volatile ("sti");
    kprint("CPU: Interrupciones de hardware habilitadas (STI OK)\n");
    kprint("TSC: Calibrado con PIT a "); kprint_dec((uint32_t)tsc_freq_mhz);
    kprint(" MHz ("); kprint_dec((uint32_t)tsc_ticks_per_ms); kprint(" ticks/ms)\n");

    clear_screen();

    pci_scan();
    virtio_blk_init();
    vfs_init();

    net_run_llm_test();

    boot_gate_check();

    vga_print_at("MYOS 0.1", 5, 36);
    vga_print_at("x86_64 kernel: LONG MODE OK", 7, 27);
    vga_print_at("VirtIO-NET: OK  |  TCP/IP: OK  |  HTTP: OK", 9, 20);
    vga_print_at("LLM Server: 192.168.1.200:8087 (nail-35b)", 11, 20);

    if (llm_test_passed) {
        vga_print_at("LLM Response: ", 14, 20);
        vga_print_at(llm_last_reply, 14, 34);
        vga_print_at(">> SYSTEM STATUS: AUTONOMOUS AI LINK ESTABLISHED <<", 17, 15);
    } else {
        vga_print_at("LLM Test: FAILED", 14, 32);
    }

    serial_print("\nMYOS 0.1\n");
    serial_print("x86_64 kernel: LONG MODE OK\n");
    if (llm_test_passed) {
        serial_print("SYSTEM STATUS: AUTONOMOUS AI LINK ESTABLISHED\n");
    }

    agent_state_load();

    shell_run();
}