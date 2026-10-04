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
    while (!serial_ready()) {
    }

    outb(COM1, (uint8_t)c);
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
    for (uint32_t i = 0; i < 80 * 25; ++i) {
        VGA[i] = 0x0720;
    }
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

/* Ejecuta la herramienta y captura un resumen de salida para retroalimentar a la IA */
static int execute_tool_with_feedback(const char *cmd_line, char *feedback_out, uint32_t max_fb)
{
    while (*cmd_line == ' ') cmd_line++;

    if (src_tool(cmd_line, feedback_out, max_fb)) {
        return 1;
    }

    if (cmd_line[0] == 'l' && cmd_line[1] == 's') {
        vfs_list();
        vfs_format_list(feedback_out, max_fb);
        return 1;
    } else if (cmd_line[0] == 'c' && cmd_line[1] == 'a' && cmd_line[2] == 't') {
        const char *p = cmd_line + 3;
        while (*p == ' ') p++;
        char filename[48];
        uint32_t fn_i = 0;
        while (*p && *p != ' ' && fn_i < sizeof(filename) - 1) {
            filename[fn_i++] = *p++;
        }
        filename[fn_i] = '\0';
        char buf[1024];
        int r = vfs_read(filename, buf, sizeof(buf));
        if (r >= 0) {
            kprint("\n[CONTENIDO DE "); kprint(filename); kprint("]:\n");
            kprint(buf); kprint("\n");
            uint32_t i = 0; while (buf[i] && i < max_fb - 1) { feedback_out[i] = buf[i]; i++; } feedback_out[i] = '\0';
        } else {
            kprint("Error: archivo no encontrado.\n");
            const char *err = "Error: archivo no encontrado en RamFS.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'w' && cmd_line[1] == 'r' && cmd_line[2] == 'i' && cmd_line[3] == 't' && cmd_line[4] == 'e') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char filename[48];
        uint32_t fn_i = 0;
        while (*p && *p != ' ' && fn_i < sizeof(filename) - 1) {
            filename[fn_i++] = *p++;
        }
        filename[fn_i] = '\0';
        while (*p == ' ') p++;
        uint32_t tlen = 0; while (p[tlen]) tlen++;
        vfs_write(filename, p, tlen);
        kprint("Archivo '"); kprint(filename); kprint("' escrito con exito.\n");
        const char *succ = "Archivo escrito correctamente en el sistema de archivos.";
        uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'r' && cmd_line[1] == 'm' && cmd_line[2] == ' ') {
        const char *fn = cmd_line + 3;
        while (*fn == ' ') fn++;
        int r = vfs_delete(fn);
        if (r == 0) {
            kprint("Archivo '"); kprint(fn); kprint("' eliminado con exito.\n");
            const char *succ = "Archivo eliminado correctamente de RamFS.";
            uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        } else {
            kprint("Error: no se pudo eliminar '"); kprint(fn); kprint("'.\n");
            const char *err = "Error: no se pudo eliminar, el archivo no existe.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
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
        m = " (Conectividad y trafico activos)"; while (*m) tmp[p++] = *m++;
        tmp[p] = '\0';
        for (uint32_t i = 0; i < p && i < max_fb - 1; ++i) feedback_out[i] = tmp[i];
        feedback_out[p < max_fb ? p : max_fb - 1] = '\0';
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'i' && cmd_line[2] == 'n' && cmd_line[3] == 'g') {
        const char *arg = cmd_line + 4;
        while (*arg == ' ') arg++;
        uint8_t target[4];
        if (*arg) {
            parse_ip(arg, target);
        } else {
            target[0] = net_gateway[0]; target[1] = net_gateway[1];
            target[2] = net_gateway[2]; target[3] = net_gateway[3];
        }
        kprint("PING a "); kprint_ip(target); kprint("...\n");
        int r = icmp_ping(target, 1, 2000);
        if (r == 1) {
            const char *succ = "Ping exitoso: respuesta ICMP recibida del host objetivo.";
            uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        } else {
            const char *fail_msg = "Ping fallido: tiempo de espera agotado, no hubo respuesta.";
            uint32_t i = 0; while (fail_msg[i] && i < max_fb - 1) { feedback_out[i] = fail_msg[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'r') {
        const char *p = cmd_line + 11;
        uint64_t sec = parse_num(&p);
        char sbuf[512];
        if (virtio_blk_read(sec, sbuf) == 0) {
            kprint("\n[SECTOR LBA "); kprint_dec((uint32_t)sec); kprint("]:\n");
            for(int i=0; i<512 && sbuf[i]; i++) {
                if (sbuf[i] >= 32 && sbuf[i] < 127) kputc(sbuf[i]);
            }
            kprint("\n");
            uint32_t i = 0; while (sbuf[i] && i < max_fb - 1 && i < 511) { feedback_out[i] = sbuf[i]; i++; } feedback_out[i] = '\0';
        } else {
            const char *err = "Error de I/O al leer el disco duro.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'w') {
        const char *p = cmd_line + 12;
        uint64_t sec = parse_num(&p);

        if (sec < SECTOR_USER_MIN) {
            kprint("Error: sectores 0-1023 reservados al sistema.\n");
            const char *err = "Error: sector protegido (LBA < 1024 reservado al sistema). Usa LBA >= 1024.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
            return 1;
        }
        while (*p == ' ') p++;
        char sbuf[512];
        for (int i=0; i<512; i++) sbuf[i] = 0;
        int bi=0;
        while (*p && bi < 511) sbuf[bi++] = *p++;
        if (virtio_blk_write(sec, sbuf) == 0) {
            kprint("Sector LBA escrito en disco persistente.\n");
            uint32_t pos = 0;
            fb_puts(feedback_out, max_fb, &pos, "Sector LBA ");
            fb_put_dec(feedback_out, max_fb, &pos, (uint32_t)sec);
            fb_puts(feedback_out, max_fb, &pos, " escrito con exito (");
            fb_put_dec(feedback_out, max_fb, &pos, (uint32_t)bi);
            fb_puts(feedback_out, max_fb, &pos, " B datos). Contenido inicial: '");
            for (int k = 0; k < bi && k < 40; ++k) {
                fb_putc(feedback_out, max_fb, &pos, sbuf[k]);
            }
            if (bi > 40) fb_puts(feedback_out, max_fb, &pos, "...");
            fb_puts(feedback_out, max_fb, &pos, "'");
        } else {
            const char *err = "Error de I/O al escribir en disco duro.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'h' && cmd_line[1] == 'o' && cmd_line[2] == 's' && cmd_line[3] == 't' && cmd_line[4] == '_' && cmd_line[5] == 'p') {
        const char *p = cmd_line + 11;
        while (*p == ' ') p++;
        char fn[56];
        int fi = 0;
        while (*p && *p != ' ' && fi < 55) fn[fi++] = *p++;
        fn[fi] = '\0';
        while (*p == ' ') p++;
        
        uint32_t clen = 0;
        while (p[clen]) clen++;
        
        char mailbox[8192];
        for(int i=0; i<8192; i++) mailbox[i] = 0;
        
        char *m = mailbox;
        m[0]='P'; m[1]='A'; m[2]='T'; m[3]='C'; m[4]='H'; m[5]='v'; m[6]='0'; m[7]='1';
        for(int i=0; i<56 && fn[i]; i++) m[8+i] = fn[i];
        
        m[64] = (char)(clen & 0xFF);
        m[65] = (char)((clen >> 8) & 0xFF);
        m[66] = (char)((clen >> 16) & 0xFF);
        m[67] = (char)((clen >> 24) & 0xFF);
        
        for(uint32_t i=0; i<clen && i < 8192 - 68; i++) {
            m[68+i] = p[i];
        }
        
        /* Escribir 16 sectores (8 KiB) empezando en LBA 1 */
        for (int s=0; s<16; s++) {
            virtio_blk_write(1 + s, mailbox + (s * 512));
        }
        
        kprint("\n\n========================================================================\n");
        kprint("!!! INICIANDO PUENTE HOST-BRIDGE: RECOMPILACION AUTONOMA EN CURSO !!!\n");
        kprint("========================================================================\n");
        kprint("Escribiendo parche en el buzon de disco y deteniendo el kernel...\n");
        
        /* Triple Fault intencionado para cerrar QEMU limpiamente */
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = {0, 0};
        __asm__ volatile ("lidt %0; int3" :: "m"(idtr));
        
        return 1;
    } else if (cmd_line[0] == 'm' && cmd_line[1] == 'e' && cmd_line[2] == 'm') {
        sysinfo_print_mem();
        sysinfo_format_mem(feedback_out, max_fb);
        return 1;
    } else if (cmd_line[0] == 'a' && cmd_line[1] == 'r' && cmd_line[2] == 'p') {
        arp_format_cache(feedback_out, max_fb);
        kprint("\n");
        kprint(feedback_out);
        kprint("\n");
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'c' && cmd_line[2] == 'i') {
        pci_scan();
        pci_format_scan(feedback_out, max_fb);
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
    return 0;
}

static int extract_valid_cmd(const char *text, char *out_cmd, uint32_t max)
{
    const char *p = text;
    while (*p) {
        const char *tag = find_substr(p, "CMD:");
        if (!tag) {
            return 0;
        }
        const char *cand = tag + 4;
        while (*cand == ' ' || *cand == '`' || *cand == '\'' || *cand == '"') cand++;

        if (is_valid_tool(cand)) {
            uint32_t i = 0;
            while (cand[i] && cand[i] != '\r' && cand[i] != '\n' &&
                   cand[i] != '`' && cand[i] != '\'' && cand[i] != '"' && i < max - 1) {
                out_cmd[i] = cand[i];
                i++;
            }
            out_cmd[i] = '\0';
            return 1;
        }
        p = tag + 4;
    }
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

/* Buffers de agente en BSS para blindar la pila */
static char agent_prompt_buf[8192];
static char agent_reply_buf[4096];
static char tool_feedback_buf[2048];

static void shell_run(void)
{
    char cmd[512];
    char reply[4096];

    serial_print("\n============================================================\n");
    serial_print("MYOS 0.1 INTERACTIVE SHELL\n");
    serial_print("Escribe 'help' para ver comandos disponibles.\n");
    serial_print("============================================================\n\n");

    for (;;) {
        serial_print("myos> ");
        int len = kgetline(cmd, sizeof(cmd));
        if (len == 0) {
            continue;
        }

        if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && (cmd[4] == '\0' || cmd[4] == ' ')) {
            serial_print("Comandos disponibles:\n");
            serial_print("Comandos disponibles:\n");
            serial_print("  help                 - Muestra esta ayuda\n");
            serial_print("  health               - Verifica estado del servidor LLM\n");
            serial_print("  llm <mensaje>        - Consulta general a nail-35b\n");
            serial_print("  llm-diag [pregunta]  - Telemetria + Diagnostico del kernel por IA\n");
            serial_print("  agent <mision>       - Agente autonomo con ejecucion de herramientas\n");
            serial_print("  heap                 - Estado de la memoria dinamica kmalloc\n");
            serial_print("  ping <ip>            - Envia ICMP echo a una direccion IPv4\n");
            serial_print("  ls                   - Lista los archivos del RamFS\n");
            serial_print("  cat <archivo>        - Muestra el contenido de un archivo\n");
            serial_print("  write <arch> <texto> - Crea o sobrescribe un archivo\n");
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

            /* Paso inicial: preparar contexto y mision */
            uint32_t ap_len = 0;
            const char *instr = "Mision: '";
            while (*instr) agent_prompt_buf[ap_len++] = *instr++;
            const char *m = mission;
            while (*m && ap_len < sizeof(agent_prompt_buf) - 500) agent_prompt_buf[ap_len++] = *m++;
            const char *ctx = "'. Contexto MYOS: Kernel x86_64 bare-metal con RamFS en /. Herramientas: stats, mem, arp, pci, ping 10.0.2.2, ls, cat /arch, write /arch texto, rm /arch. Reglas obligatorias: 1) Si la mision requiere medir o consultar antes de guardar (ej: ping y luego write), ejecuta SIEMPRE primero la herramienta de medicion (ping/stats) y en el siguiente paso guarda los datos con write. 2) Para ejecutar usa 'CMD: <herramienta>'. 3) Al concluir responde con tu dictamen final sin CMD.";
            while (*ctx) agent_prompt_buf[ap_len++] = *ctx++;
            agent_prompt_buf[ap_len] = '\0';

            /* Bucle ReAct con Salida Estructurada JSON y Grammar */
            static char history_buf[4096];
            uint32_t hist_len = 0;
            history_buf[0] = '\0';

            int finished = 0;
            for (int step = 1; step <= 8; ++step) {
                ap_len = 0;
                const char *p_m = "MISION: '"; while (*p_m) agent_prompt_buf[ap_len++] = *p_m++;
                const char *p_mval = mission; while (*p_mval && ap_len < sizeof(agent_prompt_buf) - 1200) agent_prompt_buf[ap_len++] = *p_mval++;
                const char *p_ctx = "'.\nContexto: Kernel bare-metal x86_64, RamFS en /.\n"
                                    "Responde SIEMPRE con un objeto JSON valido con este esquema exacto:\n"
                                    "{\n"
                                    "  \"thought\": \"analisis breve del paso\",\n"
                                    "  \"action\": \"tool\" | \"patch\" | \"final\",\n"
                                    "  \"cmd\": \"herramienta con args si action==tool\",\n"
                                    "  \"patch\": \"bloques FILE/SEARCH/REPLACE si action==patch\",\n"
                                    "  \"verdict\": \"dictamen final al usuario si action==final\"\n"
                                    "}\n"
                                    "Herramientas validas para cmd: stats | mem | arp | pci | ping <ip> | ls | cat /archivo | write /archivo texto | rm /archivo | sector_read LBA | sector_write LBA texto | src_ls | src_cat archivo.c OFFSET | src_grep archivo.c texto.\n"
                                    "Para modificar fuentes: usa src_grep y src_cat para leer el SEARCH exacto, y luego emite action: patch.\n";
                while (*p_ctx) agent_prompt_buf[ap_len++] = *p_ctx++;

                if (hist_len > 0) {
                    const char *h_hdr = "Historial previo:\n";
                    while (*h_hdr) agent_prompt_buf[ap_len++] = *h_hdr++;
                    for (uint32_t h = 0; h < hist_len && ap_len < sizeof(agent_prompt_buf) - 400; ++h) {
                        agent_prompt_buf[ap_len++] = history_buf[h];
                    }
                }

                const char *p_rules = "\nInstruccion: Genera el JSON correspondiente para el paso actual.";
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

                /* 1. Accion: Parche al host */
                if (find_substr(act, "patch") || patch_field[0] != '\0') {
                    const char *ptext = patch_field[0] ? patch_field : agent_reply_buf;
                    const char *pfile = find_substr(ptext, "FILE: src/");
                    if (pfile) {
                        serial_print("\n[AGENTE AUTONOMO]: Parche estructurado emitido. Enviando al host...\n");
                        host_submit_patch(pfile);
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

                /* 3. Accion: Dictamen final */
                serial_print("\n[AGENTE DICTAMEN FINAL]:\n");
                if (verdict[0]) {
                    serial_print(verdict);
                } else {
                    serial_print(agent_reply_buf);
                }
                serial_print("\n\n");
                finished = 1;
                break;
            }            if (!finished) {
                serial_print("\nMision concluida.\n\n");
            }
        } else if (cmd[0] == 'l' && cmd[1] == 's' && (cmd[2] == '\0' || cmd[2] == ' ')) {
            vfs_list();
        } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == ' ') {
            const char *fn = cmd + 4;
            while (*fn == ' ') fn++;
            char buf[2048];
            int r = vfs_read(fn, buf, sizeof(buf));
            if (r >= 0) {
                serial_print("\n");
                serial_print(buf);
                serial_print("\n");
            } else {
                serial_print("Error: archivo no encontrado: '");
                serial_print(fn);
                serial_print("'\n");
            }
        } else if (cmd[0] == 'w' && cmd[1] == 'r' && cmd[2] == 'i' && cmd[3] == 't' && cmd[4] == 'e' && cmd[5] == ' ') {
            const char *p = cmd + 6;
            while (*p == ' ') p++;
            char fn[48];
            uint32_t fi = 0;
            while (*p && *p != ' ' && fi < sizeof(fn) - 1) {
                fn[fi++] = *p++;
            }
            fn[fi] = '\0';
            while (*p == ' ') p++;
            uint32_t tlen = 0; while (p[tlen]) tlen++;
            vfs_write(fn, p, tlen);
            serial_print("Escrito '"); serial_print(fn); serial_print("'\n");
        } else if (cmd[0] == 'r' && cmd[1] == 'm' && cmd[2] == ' ') {
            const char *fn = cmd + 3;
            while (*fn == ' ') fn++;
            if (vfs_delete(fn) == 0) {
                serial_print("Eliminado '"); serial_print(fn); serial_print("'\n");
            } else {
                serial_print("Error: no se pudo eliminar '"); serial_print(fn); serial_print("'\n");
            }
        } else if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'a' && cmd[3] == 'p') {
            size_t used = 0;
            size_t free_b = 0;
            kheap_stats(&used, &free_b);
            kprint("\nESTADO DEL HEAP (kmalloc):\n");
            kprint("-------------------------\n");
            kprint("Base del Heap:    0x00400000 (4 MiB)\n");
            kprint("Memoria Usada:    "); kprint_dec((uint32_t)used); kprint(" bytes\n");
            kprint("Memoria Libre:    "); kprint_dec((uint32_t)(free_b / 1024)); kprint(" KiB\n");
            kprint("Capacidad total:  12 MiB\n\n");
        } else if (cmd[0] == 'p' && cmd[1] == 'i' && cmd[2] == 'n' && cmd[3] == 'g') {
            const char *arg = cmd + 4;
            while (*arg == ' ') arg++;
            uint8_t target[4];
            if (*arg) {
                parse_ip(arg, target);
            } else {
                target[0] = net_gateway[0]; target[1] = net_gateway[1];
                target[2] = net_gateway[2]; target[3] = net_gateway[3];
            }
            kprint("PING a "); kprint_ip(target); kprint("...\n");
            icmp_ping(target, 1, 1500);
        } else if (cmd[0] == 'm' && cmd[1] == 'e' && cmd[2] == 'm' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            sysinfo_print_mem();
        } else if (cmd[0] == 's' && cmd[1] == 't' && cmd[2] == 'a' && cmd[3] == 't' && cmd[4] == 's' && (cmd[5] == '\0' || cmd[5] == ' ')) {
            sysinfo_print_stats();
        } else if (cmd[0] == 'a' && cmd[1] == 'r' && cmd[2] == 'p' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            net_run_arp_test();
        } else if (cmd[0] == 'p' && cmd[1] == 'c' && cmd[2] == 'i' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            pci_scan();
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
        } else if ((cmd[0] == 's' && cmd[1] == 'r' && cmd[2] == 'c' && cmd[3] == '_') ||
                   (cmd[0] == 's' && cmd[1] == 'e' && cmd[2] == 'c' && cmd[3] == 't' && cmd[4] == 'o' && cmd[5] == 'r' && cmd[6] == '_')) {
            static char shell_fb[2048];
            execute_tool_with_feedback(cmd, shell_fb, sizeof(shell_fb));
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


/* ---- Puerta de Arranque (Canary Boot) ----------------------------- */
#define BOOT_GATE_LBA 24

static void boot_gate_check(void)
{
    static char gate_sec[512];
    if (virtio_blk_read(BOOT_GATE_LBA, gate_sec) != 0) {
        return;
    }

    if (memcmp(gate_sec, "GATE_TEST_REQ", 13) != 0) {
        return;
    }

    boot_gate_active = 1;
    kprint("\n============================================================\n");
    kprint("[PUERTA DE ARRANQUE]: Verificando salud del nuevo kernel\n");
    kprint("============================================================\n");

    /* Limpiar sector para no quedar en bucle */
    memset(gate_sec, 0, sizeof(gate_sec));
    virtio_blk_write(BOOT_GATE_LBA, gate_sec);

    if (!llm_health()) {
        kprint("[PUERTA DE ARRANQUE] ERROR: /health no responde.\n");
        qemu_exit(0x22);
        return;
    }

    static char canary_reply[128];
    if (!llm_chat("Responde: OK", canary_reply, sizeof(canary_reply), 15000)) {
        kprint("[PUERTA DE ARRANQUE] ERROR: chat completion fallo.\n");
        qemu_exit(0x22);
        return;
    }

    kprint("[PUERTA DE ARRANQUE] EXITO: Kernel verificado y enlace LLM activo.\n");
    kprint("Notificando al host (aprobacion de parche)...\n");
    boot_gate_active = 0;
    qemu_exit(0x20); /* Codigo de salida 65 */
}
void kernel_main(void)
{
    serial_init();
    idt_init();

    clear_screen();

    pci_scan();
    vfs_init();
    virtio_blk_init();

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

    shell_run();
}