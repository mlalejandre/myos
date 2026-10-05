#include "arch.h"
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
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "mutex.h"
#include "fs.h"
#include "io.h"
#include "srcfs.h"
#include "idt.h"
#include "boot_gate.h"

extern uint32_t multiboot_magic;
extern uint32_t multiboot_info_addr;

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

/* serial_ready unificado en console.c */

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

/* vga_print_at retirada: salida unificada por somafetch */

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
    if (!out || max == 0) return;
    if (*pos + 1 < max) {
        out[(*pos)++] = c;
        out[*pos] = '\0';
    }
}

static void fb_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    if (!out || max == 0 || !pos || *pos >= max - 1) return;
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


static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int cmos_is_updating(void)
{
    outb(0x70, 0x0A);
    return (inb(0x71) & 0x80);
}

static uint8_t bcd2bin(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
};

static void rtc_get_datetime(struct rtc_time *t)
{
    while (cmos_is_updating()) cpu_pause();
    t->sec   = cmos_read(0x00);
    t->min   = cmos_read(0x02);
    t->hour  = cmos_read(0x04);
    t->day   = cmos_read(0x07);
    t->month = cmos_read(0x08);
    uint8_t yr = cmos_read(0x09);
    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {
        t->sec   = bcd2bin(t->sec);
        t->min   = bcd2bin(t->min);
        t->hour  = bcd2bin(t->hour & 0x7F) | (t->hour & 0x80);
        t->day   = bcd2bin(t->day);
        t->month = bcd2bin(t->month);
        yr       = bcd2bin(yr);
    }
    if (!(reg_b & 0x02) && (t->hour & 0x80)) {
        t->hour = (uint8_t)(((t->hour & 0x7F) + 12) % 24);
    }
    t->year = 2000 + yr;
}

#define NANO_MAX_SIZE 4096
static char nano_buf[NANO_MAX_SIZE];

static void nano_draw_bar(uint32_t row, const char *text, uint8_t attr)
{
    uint32_t col = 0;
    while (text[col] && col < 80) {
        VGA[row * 80 + col] = (uint16_t)((attr << 8) | (uint8_t)text[col]);
        col++;
    }
    while (col < 80) {
        VGA[row * 80 + col] = (uint16_t)((attr << 8) | ' ');
        col++;
    }
}

static void run_nano(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        kprint("Uso: nano <archivo>\n");
        return;
    }

    int r = vfs_read(filename, nano_buf, sizeof(nano_buf) - 1);
    uint32_t buf_len = (r > 0) ? (uint32_t)r : 0;
    nano_buf[buf_len] = '\0';

    uint32_t cursor_pos = 0;
    uint32_t top_line = 0;
    int dirty = 0;
    char status_msg[64];
    status_msg[0] = '\0';

    console_clear();

    for (;;) {
        /* 1. Calcular linea y columna del cursor */
        uint32_t cur_line = 0;
        uint32_t cur_col = 0;
        for (uint32_t i = 0; i < cursor_pos; ++i) {
            if (nano_buf[i] == '\n') {
                cur_line++;
                cur_col = 0;
            } else {
                cur_col++;
            }
        }

        /* Ajustar scroll vertical */
        if (cur_line < top_line) top_line = cur_line;
        if (cur_line >= top_line + 22) top_line = cur_line - 21;

        /* 2. Dibujar barra superior */
        char hdr[80]; uint32_t hp = 0;
        const char *h1 = " [ SOMA nano 0.2 ]  Archivo: "; while (*h1) hdr[hp++] = *h1++;
        const char *h2 = filename; while (*h2 && hp < 50) hdr[hp++] = *h2++;
        const char *h3 = dirty ? "  [Modificado]" : "  [Limpio]"; while (*h3 && hp < 79) hdr[hp++] = *h3++;
        hdr[hp] = '\0';
        nano_draw_bar(0, hdr, 0x70);

        /* 3. Renderizar texto (filas 1 a 22) */
        uint32_t scan_l = 0;
        uint32_t p = 0;
        while (p < buf_len && scan_l < top_line) {
            if (nano_buf[p++] == '\n') scan_l++;
        }

        for (uint32_t row = 1; row <= 22; ++row) {
            uint32_t col = 0;
            if (p < buf_len && scan_l >= top_line) {
                while (p < buf_len && nano_buf[p] != '\n') {
                    if (col < 80) {
                        VGA[row * 80 + col] = (uint16_t)(0x0700 | (uint8_t)nano_buf[p]);
                        col++;
                    }
                    p++;
                }
                if (p < buf_len && nano_buf[p] == '\n') p++;
                scan_l++;
            }
            while (col < 80) {
                VGA[row * 80 + col] = 0x0720;
                col++;
            }
        }

        /* 4. Dibujar barra de estado (fila 23) */
        char st[80]; uint32_t sp = 0;
        if (status_msg[0]) {
            const char *s = status_msg; while (*s && sp < 79) st[sp++] = *s++;
        } else {
            const char *s = "Linea: "; while (*s) st[sp++] = *s++;
            uint32_t val = cur_line + 1; char tb[10]; int tn=0; while(val){tb[tn++]='0'+(val%10); val/=10;} while(tn) st[sp++]=tb[--tn];
            s = ", Col: "; while (*s) st[sp++] = *s++;
            val = cur_col + 1; tn=0; while(val){tb[tn++]='0'+(val%10); val/=10;} while(tn) st[sp++]=tb[--tn];
            s = " | Tamano: "; while (*s) st[sp++] = *s++;
            val = buf_len; if(val==0) st[sp++]='0'; else { tn=0; while(val){tb[tn++]='0'+(val%10); val/=10;} while(tn) st[sp++]=tb[--tn]; }
            s = " B"; while (*s) st[sp++] = *s++;
        }
        st[sp] = '\0';
        nano_draw_bar(23, st, 0x0F);

        /* 5. Dibujar atajos (fila 24) */
        nano_draw_bar(24, " ^S Guardar    ^X Salir    ^P/^N Arriba/Abajo    ^B/^F Izq/Der ", 0x70);

        /* 6. Mover cursor de hardware */
        uint32_t scr_row = 1 + (cur_line - top_line);
        uint32_t scr_col = (cur_col < 80) ? cur_col : 79;
        uint16_t cpos = (uint16_t)(scr_row * 80 + scr_col);
        outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(cpos & 0xFF));
        outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((cpos >> 8) & 0xFF));

        /* 7. Procesar tecla */
        char ch = kgetc();
        status_msg[0] = '\0';

        if (ch == 24) { /* Ctrl+X: Salir */
            break;
        } else if (ch == 19) { /* Ctrl+S: Guardar */
            if (vfs_write(filename, nano_buf, buf_len) >= 0) {
                dirty = 0;
                const char *ok = "[Guardado con exito en RamFS / virtio-blk]";
                uint32_t k = 0; while (*ok) status_msg[k++] = *ok++; status_msg[k] = '\0';
            } else {
                const char *err = "[Error de I/O al guardar archivo]";
                uint32_t k = 0; while (*err) status_msg[k++] = *err++; status_msg[k] = '\0';
            }
            continue;
        } else if (ch == 2) { /* Ctrl+B o Flecha Izq */
            if (cursor_pos > 0) cursor_pos--;
        } else if (ch == 6) { /* Ctrl+F o Flecha Der */
            if (cursor_pos < buf_len) cursor_pos++;
        } else if (ch == 16) { /* Ctrl+P o Flecha Arriba */
            if (cur_line > 0) {
                uint32_t target_col = cur_col;
                while (cursor_pos > 0 && nano_buf[cursor_pos - 1] != '\n') cursor_pos--;
                if (cursor_pos > 0) cursor_pos--; /* saltar el '\n' */
                uint32_t line_start = cursor_pos;
                while (line_start > 0 && nano_buf[line_start - 1] != '\n') line_start--;
                uint32_t line_len = cursor_pos - line_start;
                cursor_pos = line_start + ((target_col < line_len) ? target_col : line_len);
            }
        } else if (ch == 14) { /* Ctrl+N o Flecha Abajo */
            uint32_t target_col = cur_col;
            uint32_t next_line = cursor_pos;
            while (next_line < buf_len && nano_buf[next_line] != '\n') next_line++;
            if (next_line < buf_len) {
                next_line++; /* avanzar tras '\n' */
                uint32_t line_len = 0;
                while (next_line + line_len < buf_len && nano_buf[next_line + line_len] != '\n') line_len++;
                cursor_pos = next_line + ((target_col < line_len) ? target_col : line_len);
            }
        } else if (ch == '\b' || ch == 127) { /* Backspace */
            if (cursor_pos > 0) {
                for (uint32_t i = cursor_pos - 1; i < buf_len; ++i) {
                    nano_buf[i] = nano_buf[i + 1];
                }
                buf_len--;
                cursor_pos--;
                dirty = 1;
            }
        } else if (ch == '\r' || ch == '\n') { /* Enter */
            if (buf_len < NANO_MAX_SIZE - 2) {
                for (uint32_t i = buf_len + 1; i > cursor_pos; --i) {
                    nano_buf[i] = nano_buf[i - 1];
                }
                nano_buf[cursor_pos] = '\n';
                buf_len++;
                cursor_pos++;
                nano_buf[buf_len] = '\0';
                dirty = 1;
            }
        } else if ((uint8_t)ch >= 32 && (uint8_t)ch < 127) { /* Caracter imprimible */
            if (buf_len < NANO_MAX_SIZE - 2) {
                for (uint32_t i = buf_len + 1; i > cursor_pos; --i) {
                    nano_buf[i] = nano_buf[i - 1];
                }
                nano_buf[cursor_pos] = ch;
                buf_len++;
                cursor_pos++;
                nano_buf[buf_len] = '\0';
                dirty = 1;
            }
        }
    }

    console_clear();
    kprint("[nano: sesion finalizada para '"); kprint(filename); kprint("']\n");
}

static void system_reboot(void)
{
    kprint("\n[REBOOT] Sincronizando discos...\n");
    vfs_sync();
    kprint("[REBOOT] Reiniciando procesador (8042 reset)...\n");
    for (int i = 0; i < 10000; ++i) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idtr = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idtr));
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

static void system_poweroff(void)
{
    kprint("\n[POWEROFF] Sincronizando discos...\n");
    vfs_sync();
    kprint("[POWEROFF] Apagando maquina (ACPI)...\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outb(0xF4, 0x00);
    kprint("Apagado completado. Deteniendo CPU.\n");
    for (;;) { __asm__ volatile ("cli; hlt"); }
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
    if (limit > 128) limit = 128; /* Limite razonable para buffer de telemetria IA */

    kprint("\nHEXDUMP de '"); kprint(filename); kprint("' ("); kprint_dec(total); kprint(" bytes):\n");
    kprint("Offset    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  |ASCII           |\n");
    kprint("--------  -----------------------  -----------------------  |----------------|\n");

    const char hexchars[] = "0123456789ABCDEF";
    for (uint32_t off = 0; off < limit; off += 16) {
        for (int sh = 28; sh >= 0; sh -= 4) {
            char hc = hexchars[(off >> sh) & 0x0F];
            kputc(hc);
            if (out_buf && pos < max_out - 1) out_buf[pos++] = hc;
        }
        kprint(": ");
        if (out_buf && pos < max_out - 2) { out_buf[pos++] = ':'; out_buf[pos++] = ' '; }

        for (uint32_t i = 0; i < 16; ++i) {
            if (off + i < limit) {
                uint8_t b = (uint8_t)hbuf[off + i];
                char h1 = hexchars[(b >> 4) & 0x0F];
                char h2 = hexchars[b & 0x0F];
                kputc(h1); kputc(h2); kputc(' ');
                if (out_buf && pos < max_out - 3) {
                    out_buf[pos++] = h1; out_buf[pos++] = h2; out_buf[pos++] = ' ';
                }
            } else {
                kprint("   ");
                if (out_buf && pos < max_out - 3) {
                    out_buf[pos++] = ' '; out_buf[pos++] = ' '; out_buf[pos++] = ' ';
                }
            }
            if (i == 7) {
                kputc(' ');
                if (out_buf && pos < max_out - 1) out_buf[pos++] = ' ';
            }
        }
        kprint(" |");
        if (out_buf && pos < max_out - 2) { out_buf[pos++] = ' '; out_buf[pos++] = '|'; }

        for (uint32_t i = 0; i < 16 && off + i < limit; ++i) {
            uint8_t c = (uint8_t)hbuf[off + i];
            char printable = (c >= 32 && c < 127) ? (char)c : '.';
            kputc(printable);
            if (out_buf && pos < max_out - 1) out_buf[pos++] = printable;
        }
        kprint("|\n");
        if (out_buf && pos < max_out - 2) { out_buf[pos++] = '|'; out_buf[pos++] = '\n'; }
    }
    if (out_buf) out_buf[pos] = '\0';
    kprint("\n");
}

static void show_somafetch(char *out_buf, uint32_t max_out)
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

    uint32_t files_u = 0, bytes_u = 0, dirty_c = 0;
    vfs_get_stats(&files_u, &bytes_u, &dirty_c);

    kprint("\n");
    fb_puts(out_buf, max_out, &pos, "\n");

    /* Tipografia original restaurada, acolchada exactamente a 31 columnas */
    kprint("\033[1;36m  ____  ____  __  __    _      \033[1;33mOS:     \033[1;37mSOMA v0.2 \033[0;36m(Multi-Agente)\033[0m\n");
    fb_puts(out_buf, max_out, &pos, "  ____  ____  __  __    _      OS:     SOMA v0.2 (Multi-Agente)\n");

    kprint("\033[1;36m / ___|/ __ \\|  \\/  |  / \\     \033[1;33mKernel: \033[1;37mx86_64 Bare-Metal \033[1;32m(HAL v1)\033[0m\n");
    fb_puts(out_buf, max_out, &pos, " / ___|/ __ \\|  \\/  |  / \\     Kernel: x86_64 Bare-Metal (HAL v1)\n");

    kprint("\033[1;36m \\___ \\ |  | | |\\/| | / _ \\    \033[1;33mUptime: \033[1;37m");
    fb_puts(out_buf, max_out, &pos, " \\___ \\ |  | | |\\/| | / _ \\    Uptime: ");
    if (h > 0) { kprint_dec(h); kprint("h "); fb_put_dec(out_buf, max_out, &pos, h); fb_puts(out_buf, max_out, &pos, "h "); }
    if (m > 0 || h > 0) { kprint_dec(m); kprint("m "); fb_put_dec(out_buf, max_out, &pos, m); fb_puts(out_buf, max_out, &pos, "m "); }
    kprint_dec(s); kprint("s \033[0;32m(PIT 1 kHz)\033[0m\n");
    fb_put_dec(out_buf, max_out, &pos, s); fb_puts(out_buf, max_out, &pos, "s (PIT 1 kHz)\n");

    kprint("\033[1;36m  ___)| |__| | |  | |/ ___ \\  \033[1;33m CPU:    \033[1;37mRing 0 \033[1;35m(CR0:WP, NXE)\033[0m\n");
    fb_puts(out_buf, max_out, &pos, "  ___) | |__| | |  | |/ ___ \\  CPU:    Ring 0 (CR0:WP, NXE)\n");

    kprint("\033[1;36m |____/\\____/|_|  |_/_/   \\_\\  \033[1;33mRAM:    \033[1;37m");
    fb_puts(out_buf, max_out, &pos, " |____/\\____/|_|  |_/_/   \\_\\  RAM:    ");
    kprint_dec(ram_usd); kprint("/"); kprint_dec(ram_tot); kprint(" MiB \033[1;32m("); kprint_dec(ram_pct); kprint("% PMM)\033[0m\n");
    fb_put_dec(out_buf, max_out, &pos, ram_usd); fb_puts(out_buf, max_out, &pos, "/");
    fb_put_dec(out_buf, max_out, &pos, ram_tot); fb_puts(out_buf, max_out, &pos, " MiB (");
    fb_put_dec(out_buf, max_out, &pos, ram_pct); fb_puts(out_buf, max_out, &pos, "% PMM)\n");

    kprint("                               \033[1;33mHeap:   \033[1;37m");
    fb_puts(out_buf, max_out, &pos, "                               Heap:   ");
    kprint_dec((uint32_t)(h_used / 1024)); kprint(" KiB / \033[1;32m"); kprint_dec((uint32_t)(h_free / 1024)); kprint(" KiB libres\033[0m\n");
    fb_put_dec(out_buf, max_out, &pos, (uint32_t)(h_used / 1024)); fb_puts(out_buf, max_out, &pos, " KiB / ");
    fb_put_dec(out_buf, max_out, &pos, (uint32_t)(h_free / 1024)); fb_puts(out_buf, max_out, &pos, " KiB libres\n");

    kprint("                               \033[1;33mDisco:  \033[1;37mRamFS \033[0;36m(16 MiB virtio-blk)\033[0m\n");
    fb_puts(out_buf, max_out, &pos, "                               Disco:  RamFS (16 MiB virtio-blk)\n");

    kprint("                               \033[1;33mRed:    \033[1;37m10.0.2.15 \033[0;32m(VirtIO-NET NAT)\033[0m\n");
    fb_puts(out_buf, max_out, &pos, "                               Red:    10.0.2.15 (VirtIO-NET NAT)\n");

    kprint("                               \033[1;33mAgente: \033[1;37mnail-35b \033[1;35m(ReAct LAN)\033[0m\n\n");
    fb_puts(out_buf, max_out, &pos, "                               Agente: nail-35b (ReAct LAN)\n\n");
}

/* ---- Despachador unificado de comandos (Shell y Agente) ---------- */
struct bg_job {
    char cmd[256];
    uint32_t tid;
    char log_file[64];
};

static int dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out);

static void spawn_job_worker(void *arg)
{
    struct bg_job *job = (struct bg_job *)arg;
    if (!job) return;

    char *out_buf = (char *)kmalloc(2048);
    if (out_buf) {
        out_buf[0] = '\0';
        dispatch_command(job->cmd, out_buf, 2048);

        if (out_buf[0] != '\0') {
            uint32_t len = 0;
            while (out_buf[len]) len++;
            vfs_write(job->log_file, out_buf, len);
        }
        kfree(out_buf);
    }

    kprint("\n[JOB TID ");
    kprint_dec(job->tid);
    kprint(" ('");
    kprint(job->cmd);
    kprint("') FINALIZADO -> Salida: ");
    kprint(job->log_file);
    kprint("]\nsoma> ");

    kfree(job);
}

static int dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out)
{
    static int redir_active = 0;
    static char redir_buf[4096];

    /* Interceptor de Redireccion Universal ('>') */
    if (!redir_active) {
        int gt_idx = -1;
        int in_q = 0;
        for (int i = 0; cmd_line[i]; ++i) {
            if (cmd_line[i] == '\'' || cmd_line[i] == '"') in_q = !in_q;
            else if (!in_q && cmd_line[i] == '>') { gt_idx = i; break; }
        }
        if (gt_idx >= 0) {
            char sub_cmd[256];
            char dst_file[64];
            int sp = 0, dp = 0;
            for (int i = 0; i < gt_idx && sp < 255; ++i) sub_cmd[sp++] = cmd_line[i];
            while (sp > 0 && sub_cmd[sp - 1] == ' ') sp--;
            sub_cmd[sp] = '\0';
            const char *p = cmd_line + gt_idx + 1;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && dp < 63) dst_file[dp++] = *p++;
            dst_file[dp] = '\0';
            if (sp > 0 && dp > 0) {
                redir_active = 1;
                console_muted = 1;
                redir_buf[0] = '\0';
                int res = dispatch_command(sub_cmd, redir_buf, sizeof(redir_buf));
                console_muted = 0;
                redir_active = 0;
                if (res) {
                    uint32_t rlen = 0;
                    while (redir_buf[rlen]) rlen++;
                    vfs_write(dst_file, redir_buf, rlen);
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

    if (cmd_line[0] == 'p' && cmd_line[1] == 's' && (cmd_line[2] == '\0' || cmd_line[2] == ' ')) {
        thread_dump();
        kmutex_test_self();
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "PS: Procesos e hilos verificados.");
        return 1;
    }
    if (cmd_line[0] == 't' && cmd_line[1] == 'h' && cmd_line[2] == 'r' && cmd_line[3] == 'e' && cmd_line[4] == 'a' && cmd_line[5] == 'd' && cmd_line[6] == 's') {
        thread_dump();
        thread_test_self();
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "KTHREADS: Planificador cooperativo verificado.");
        return 1;
    }

    if (cmd_line[0] == 'v' && cmd_line[1] == 'm' && cmd_line[2] == 'm' && (cmd_line[3] == '\0' || cmd_line[3] == ' ')) {
        vmm_test_self();
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "VMM: Paginacion 4 KiB verificada.");
        return 1;
    }

    if (cmd_line[0] == 'p' && cmd_line[1] == 'm' && cmd_line[2] == 'm' && (cmd_line[3] == '\0' || cmd_line[3] == ' ')) {
        pmm_dump_stats();
        pmm_test_self();
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "PMM: Bitmap allocator verificado.");
        return 1;
    }
    if (cmd_line[0] == 'c' && cmd_line[1] == 'l' && cmd_line[2] == 's') {
        console_clear();
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "Pantalla borrada con exito.");
        return 1;
    }

    if ((cmd_line[0] == 's' && cmd_line[1] == 'o' && cmd_line[2] == 'm' && cmd_line[3] == 'a' && cmd_line[4] == 'f' && cmd_line[5] == 'e' && cmd_line[6] == 't' && cmd_line[7] == 'c' && cmd_line[8] == 'h') ||
        (cmd_line[0] == 'n' && cmd_line[1] == 'e' && cmd_line[2] == 'o' && cmd_line[3] == 'f' && cmd_line[4] == 'e' && cmd_line[5] == 't' && cmd_line[6] == 'c' && cmd_line[7] == 'h') ||
        (cmd_line[0] == 'f' && cmd_line[1] == 'e' && cmd_line[2] == 't' && cmd_line[3] == 'c' && cmd_line[4] == 'h' && (cmd_line[5] == '\0' || cmd_line[5] == ' '))) {
        show_somafetch(out_buf, max_out);
        return 1;
    }

    while (*cmd_line == ' ') cmd_line++;

            if ((cmd_line[0] == 'n' && cmd_line[1] == 'a' && cmd_line[2] == 'n' && cmd_line[3] == 'o' && (cmd_line[4] == ' ' || cmd_line[4] == '\0')) ||
        (cmd_line[0] == 'e' && cmd_line[1] == 'd' && cmd_line[2] == 'i' && cmd_line[3] == 't' && (cmd_line[4] == ' ' || cmd_line[4] == '\0'))) {
        const char *p = (cmd_line[0] == 'n') ? (cmd_line + 4) : (cmd_line + 4);
        while (*p == ' ') p++;
        if (*p == '\0') {
            kprint("Uso: nano <archivo>\n");
            uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Uso: nano <archivo>");
            return 1;
        }
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        run_nano(fn);
        uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "nano cerrado.");
        return 1;
    }

    if (cmd_line[0] == 'g' && cmd_line[1] == 'r' && cmd_line[2] == 'e' && cmd_line[3] == 'p' && cmd_line[4] == ' ') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char pat[64]; uint32_t pi = 0;
        char q = 0;
        if (*p == '\'' || *p == '"') q = *p++;
        while (*p && pi < sizeof(pat) - 1) {
            if (q && *p == q) { p++; break; }
            if (!q && *p == ' ') break;
            pat[pi++] = *p++;
        }
        pat[pi] = '\0';
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        if (pi == 0 || fi == 0) {
            kprint("Uso: grep <patron> <archivo>\n");
            uint32_t pos = 0; fb_puts(out_buf, max_out, &pos, "Uso: grep <patron> <archivo>");
            return 1;
        }
        static char gbuf[4096];
        int r = vfs_read(fn, gbuf, sizeof(gbuf) - 1);
        uint32_t pos = 0;
        if (r < 0) {
            kprint("grep: archivo no encontrado: '"); kprint(fn); kprint("'\n");
            fb_puts(out_buf, max_out, &pos, "grep: archivo no encontrado");
            return 1;
        }
        gbuf[r] = '\0';
        uint32_t line_num = 1;
        uint32_t start = 0;
        int matches = 0;
        for (int i = 0; i <= r; ++i) {
            if (gbuf[i] == '\n' || gbuf[i] == '\0') {
                gbuf[i] = '\0';
                const char *line = gbuf + start;
                if (find_substr(line, pat)) {
                    matches++;
                    kprint_dec(line_num); kprint(": "); kprint(line); kprint("\n");
                    fb_put_dec(out_buf, max_out, &pos, line_num); fb_puts(out_buf, max_out, &pos, ": ");
                    fb_puts(out_buf, max_out, &pos, line); fb_puts(out_buf, max_out, &pos, "\n");
                }
                line_num++;
                start = (uint32_t)(i + 1);
            }
        }
        if (matches == 0) {
            kprint("grep: sin coincidencias.\n");
            fb_puts(out_buf, max_out, &pos, "grep: sin coincidencias.");
        }
        return 1;
    }

    if (cmd_line[0] == 'e' && cmd_line[1] == 'c' && cmd_line[2] == 'h' && cmd_line[3] == 'o' && (cmd_line[4] == ' ' || cmd_line[4] == '\0')) {
        const char *t = cmd_line + 4;
        while (*t == ' ') t++;
        char clean[512]; uint32_t ci = 0;
        char q = 0;
        if (*t == '\'' || *t == '"') q = *t++;
        while (*t && ci < sizeof(clean) - 1) {
            if (q && *t == q) { t++; break; }
            clean[ci++] = *t++;
        }
        clean[ci] = '\0';
        kprint(clean); kprint("\n");
        uint32_t pos = 0;
        fb_puts(out_buf, max_out, &pos, clean); fb_putc(out_buf, max_out, &pos, '\n');
        return 1;
    }

    if ((cmd_line[0] == 'h' && cmd_line[1] == 'e' && cmd_line[2] == 'x' && cmd_line[3] == 'd' && cmd_line[4] == 'u' && cmd_line[5] == 'm' && cmd_line[6] == 'p' && cmd_line[7] == ' ') ||
        (cmd_line[0] == 'x' && cmd_line[1] == 'x' && cmd_line[2] == 'd' && cmd_line[3] == ' ')) {
        const char *p = (cmd_line[0] == 'h') ? (cmd_line + 8) : (cmd_line + 4);
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        while (*p == ' ') p++;
        uint32_t maxb = 256;
        if (*p >= '0' && *p <= '9') {
            maxb = 0;
            while (*p >= '0' && *p <= '9') maxb = maxb * 10 + (*p++ - '0');
        }
        do_hexdump(fn, maxb, out_buf, max_out);
        return 1;
    }

    if (cmd_line[0] == 'r' && cmd_line[1] == 'e' && cmd_line[2] == 'b' && cmd_line[3] == 'o' && cmd_line[4] == 'o' && cmd_line[5] == 't') {
        system_reboot();
        return 1;
    }

    if ((cmd_line[0] == 'p' && cmd_line[1] == 'o' && cmd_line[2] == 'w' && cmd_line[3] == 'e' && cmd_line[4] == 'r' && cmd_line[5] == 'o' && cmd_line[6] == 'f' && cmd_line[7] == 'f') ||
        (cmd_line[0] == 's' && cmd_line[1] == 'h' && cmd_line[2] == 'u' && cmd_line[3] == 't' && cmd_line[4] == 'd' && cmd_line[5] == 'o' && cmd_line[6] == 'w' && cmd_line[7] == 'n')) {
        system_poweroff();
        return 1;
    }

    if (cmd_line[0] == 'h' && cmd_line[1] == 'a' && cmd_line[2] == 'l' && cmd_line[3] == 't') {
        kprint("Sincronizando disco persistente...\n");
        vfs_sync();
        kprint("Sistema detenido (HALT). Es seguro apagar el equipo.\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
        return 1;
    }

if (cmd_line[0] == 'd' && cmd_line[1] == 'a' && cmd_line[2] == 't' && cmd_line[3] == 'e' && (cmd_line[4] == '\0' || cmd_line[4] == ' ')) {
        struct rtc_time t;
        rtc_get_datetime(&t);
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

    if (cmd_line[0] == 't' && cmd_line[1] == 'i' && cmd_line[2] == 'm' && cmd_line[3] == 'e' && cmd_line[4] == ' ') {
        const char *sub = cmd_line + 5;
        while (*sub == ' ') sub++;
        if (*sub == '\0') {
            kprint("Uso: time <comando>\n");
            return 1;
        }
        uint64_t t0 = timer_get_uptime_ms();
        uint64_t c0 = rdtsc();
        dispatch_command(sub, out_buf, max_out);
        uint64_t c1 = rdtsc();
        uint64_t t1 = timer_get_uptime_ms();
        uint64_t ms = t1 - t0;
        uint64_t cycles = c1 - c0;
        kprint("\n[BENCHMARK TIME]: "); kprint_dec((uint32_t)ms); kprint(" ms | ");
        kprint_dec((uint32_t)cycles); kprint(" ciclos TSC\n");
        return 1;
    }

    if (cmd_line[0] == 'f' && cmd_line[1] == 'r' && cmd_line[2] == 'e' && cmd_line[3] == 'e' && (cmd_line[4] == '\0' || cmd_line[4] == ' ')) {
        size_t f_free = 0, f_used = 0, f_total = 0;
        pmm_get_stats(&f_free, &f_used, &f_total);
        size_t h_used = 0, h_free = 0;
        kheap_stats(&h_used, &h_free);
        uint32_t ram_tot = (uint32_t)((f_total * 4096) / (1024 * 1024));
        uint32_t ram_usd = (uint32_t)((f_used * 4096) / (1024 * 1024));
        uint32_t ram_fre = (uint32_t)((f_free * 4096) / (1024 * 1024));
        uint32_t ram_pct = f_total ? (uint32_t)((f_used * 100) / f_total) : 0;
        kprint("\nESTADO DE MEMORIA (free):\n");
        kprint("RAM Física: "); kprint_dec(ram_usd); kprint(" MiB usados / ");
        kprint_dec(ram_tot); kprint(" MiB totales ("); kprint_dec(ram_pct); kprint("% uso). Libre: ");
        kprint_dec(ram_fre); kprint(" MiB\n");
        kprint("Heap VMM:   "); kprint_dec((uint32_t)(h_used / 1024)); kprint(" KiB usados / ");
        kprint_dec((uint32_t)(h_free / 1024)); kprint(" KiB libres\n\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "RAM: ");
        fb_put_dec(out_buf, max_out, &p, ram_usd); fb_puts(out_buf, max_out, &p, "/");
        fb_put_dec(out_buf, max_out, &p, ram_tot); fb_puts(out_buf, max_out, &p, " MiB | Heap libre: ");
        fb_put_dec(out_buf, max_out, &p, (uint32_t)(h_free / 1024)); fb_puts(out_buf, max_out, &p, " KiB");
        return 1;
    }

    if (cmd_line[0] == 'd' && cmd_line[1] == 'f' && (cmd_line[2] == '\0' || cmd_line[2] == ' ')) {
        uint32_t files_u = 0, bytes_u = 0, dirty_c = 0;
        vfs_get_stats(&files_u, &bytes_u, &dirty_c);
        kprint("\nALMACENAMIENTO (df):\n");
        kprint("RamFS: "); kprint_dec(files_u); kprint("/32 archivos usados | ");
        kprint_dec(bytes_u / 1024); kprint(" KiB usados / 128 KiB capacidad\n");
        kprint("VirtIO-BLK: 16 MiB disco total | Inodos dirty pendientes: ");
        kprint_dec(dirty_c); kprint("\n\n");
        uint32_t p = 0;
        fb_puts(out_buf, max_out, &p, "RamFS: ");
        fb_put_dec(out_buf, max_out, &p, files_u); fb_puts(out_buf, max_out, &p, "/32 archivos | ");
        fb_put_dec(out_buf, max_out, &p, bytes_u / 1024); fb_puts(out_buf, max_out, &p, " KiB usados");
        return 1;
    }

    if (cmd_line[0] == 't' && cmd_line[1] == 'r' && cmd_line[2] == 'e' && cmd_line[3] == 'e' && (cmd_line[4] == '\0' || cmd_line[4] == ' ')) {
        vfs_tree(out_buf, max_out);
        return 1;
    }

    if (cmd_line[0] == 'c' && cmd_line[1] == 'p' && cmd_line[2] == ' ') {
        const char *p = cmd_line + 3;
        while (*p == ' ') p++;
        char src[48], dst[48];
        uint32_t si = 0, di = 0;
        while (*p && *p != ' ' && si < sizeof(src) - 1) src[si++] = *p++;
        src[si] = '\0';
        while (*p == ' ') p++;
        while (*p && *p != ' ' && di < sizeof(dst) - 1) dst[di++] = *p++;
        dst[di] = '\0';
        static char cp_buf[4096];
        int r = vfs_read(src, cp_buf, sizeof(cp_buf));
        uint32_t pos = 0;
        if (r < 0) {
            kprint("cp: origen no encontrado\n");
            fb_puts(out_buf, max_out, &pos, "cp: archivo origen no encontrado");
        } else if (vfs_write(dst, cp_buf, (uint32_t)r) >= 0) {
            kprint("cp: copiado '"); kprint(src); kprint("' a '"); kprint(dst); kprint("'\n");
            fb_puts(out_buf, max_out, &pos, "cp: copiado con exito");
        } else {
            kprint("cp: error al escribir destino\n");
            fb_puts(out_buf, max_out, &pos, "cp: error al escribir destino");
        }
        return 1;
    }

    if (cmd_line[0] == 'm' && cmd_line[1] == 'v' && cmd_line[2] == ' ') {
        const char *p = cmd_line + 3;
        while (*p == ' ') p++;
        char src[48], dst[48];
        uint32_t si = 0, di = 0;
        while (*p && *p != ' ' && si < sizeof(src) - 1) src[si++] = *p++;
        src[si] = '\0';
        while (*p == ' ') p++;
        while (*p && *p != ' ' && di < sizeof(dst) - 1) dst[di++] = *p++;
        dst[di] = '\0';
        static char mv_buf[4096];
        int r = vfs_read(src, mv_buf, sizeof(mv_buf));
        uint32_t pos = 0;
        if (r < 0) {
            kprint("mv: origen no encontrado\n");
            fb_puts(out_buf, max_out, &pos, "mv: origen no encontrado");
        } else if (vfs_write(dst, mv_buf, (uint32_t)r) >= 0) {
            vfs_delete(src);
            kprint("mv: movido '"); kprint(src); kprint("' a '"); kprint(dst); kprint("'\n");
            fb_puts(out_buf, max_out, &pos, "mv: movido con exito");
        } else {
            kprint("mv: error al mover\n");
            fb_puts(out_buf, max_out, &pos, "mv: error al mover");
        }
        return 1;
    }

    if (cmd_line[0] == 't' && cmd_line[1] == 'o' && cmd_line[2] == 'u' && cmd_line[3] == 'c' && cmd_line[4] == 'h' && cmd_line[5] == ' ') {
        const char *p = cmd_line + 6;
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        char tmp[8];
        uint32_t pos = 0;
        if (vfs_read(fn, tmp, sizeof(tmp)) < 0) {
            vfs_write(fn, "", 0);
            kprint("touch: creado '"); kprint(fn); kprint("'\n");
            fb_puts(out_buf, max_out, &pos, "touch: archivo creado");
        } else {
            kprint("touch: ya existe\n");
            fb_puts(out_buf, max_out, &pos, "touch: el archivo ya existia");
        }
        return 1;
    }

    if (cmd_line[0] == 'h' && cmd_line[1] == 'e' && cmd_line[2] == 'a' && cmd_line[3] == 'd' && cmd_line[4] == ' ') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        while (*p == ' ') p++;
        uint32_t lines = 10;
        if (*p >= '0' && *p <= '9') {
            lines = 0;
            while (*p >= '0' && *p <= '9') lines = lines * 10 + (*p++ - '0');
        }
        static char hbuf[4096];
        int r = vfs_read(fn, hbuf, sizeof(hbuf));
        uint32_t pos = 0;
        if (r < 0) { fb_puts(out_buf, max_out, &pos, "head: archivo no encontrado"); return 1; }
        uint32_t lcount = 0;
        kprint("\n");
        for (int i = 0; i < r && lcount < lines; ++i) {
            kputc(hbuf[i]);
            if (pos < max_out - 1) out_buf[pos++] = hbuf[i];
            if (hbuf[i] == '\n') lcount++;
        }
        out_buf[pos] = '\0';
        kprint("\n");
        return 1;
    }

    if (cmd_line[0] == 't' && cmd_line[1] == 'a' && cmd_line[2] == 'i' && cmd_line[3] == 'l' && cmd_line[4] == ' ') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        while (*p == ' ') p++;
        uint32_t lines = 10;
        if (*p >= '0' && *p <= '9') {
            lines = 0;
            while (*p >= '0' && *p <= '9') lines = lines * 10 + (*p++ - '0');
        }
        static char tbuf[4096];
        int r = vfs_read(fn, tbuf, sizeof(tbuf));
        uint32_t pos = 0;
        if (r < 0) { fb_puts(out_buf, max_out, &pos, "tail: archivo no encontrado"); return 1; }
        uint32_t total_l = 0;
        for (int i = 0; i < r; ++i) if (tbuf[i] == '\n') total_l++;
        uint32_t skip_l = (total_l > lines) ? (total_l - lines) : 0;
        uint32_t cur_l = 0, start_idx = 0;
        while (start_idx < (uint32_t)r && cur_l < skip_l) {
            if (tbuf[start_idx++] == '\n') cur_l++;
        }
        kprint("\n");
        for (uint32_t i = start_idx; i < (uint32_t)r; ++i) {
            kputc(tbuf[i]);
            if (pos < max_out - 1) out_buf[pos++] = tbuf[i];
        }
        out_buf[pos] = '\0';
        kprint("\n");
        return 1;
    }

    if (cmd_line[0] == 'w' && cmd_line[1] == 'c' && cmd_line[2] == ' ') {
        const char *p = cmd_line + 3;
        while (*p == ' ') p++;
        char fn[48]; uint32_t fi = 0;
        while (*p && *p != ' ' && fi < sizeof(fn) - 1) fn[fi++] = *p++;
        fn[fi] = '\0';
        static char wcbuf[4096];
        int r = vfs_read(fn, wcbuf, sizeof(wcbuf));
        uint32_t pos = 0;
        if (r < 0) { fb_puts(out_buf, max_out, &pos, "wc: archivo no encontrado"); return 1; }
        uint32_t l = 0, w = 0;
        int in_word = 0;
        for (int i = 0; i < r; ++i) {
            if (wcbuf[i] == '\n') l++;
            if (wcbuf[i] == ' ' || wcbuf[i] == '\t' || wcbuf[i] == '\n' || wcbuf[i] == '\r') in_word = 0;
            else if (!in_word) { in_word = 1; w++; }
        }
        kprint("  "); kprint_dec(l); kprint(" lineas  ");
        kprint_dec(w); kprint(" palabras  ");
        kprint_dec((uint32_t)r); kprint(" bytes  "); kprint(fn); kprint("\n");
        fb_put_dec(out_buf, max_out, &pos, l); fb_puts(out_buf, max_out, &pos, " lineas, ");
        fb_put_dec(out_buf, max_out, &pos, w); fb_puts(out_buf, max_out, &pos, " palabras, ");
        fb_put_dec(out_buf, max_out, &pos, (uint32_t)r); fb_puts(out_buf, max_out, &pos, " bytes");
        return 1;
    }

if ((cmd_line[0] == 's' && cmd_line[1] == 'p' && cmd_line[2] == 'a' && cmd_line[3] == 'w' && cmd_line[4] == 'n' && cmd_line[5] == ' ') ||
        (cmd_line[0] == 'b' && cmd_line[1] == 'g' && cmd_line[2] == ' ')) {
        const char *subcmd = (cmd_line[0] == 's') ? (cmd_line + 6) : (cmd_line + 3);
        while (*subcmd == ' ') subcmd++;
        if (*subcmd == '\0') {
            kprint("Uso: spawn <comando>  (alias: bg <comando>)\n");
            uint32_t p = 0;
            fb_puts(out_buf, max_out, &p, "Uso: spawn <comando>");
            return 1;
        }

        if ((subcmd[0] == 's' && subcmd[1] == 'p' && subcmd[2] == 'a' && subcmd[3] == 'w' && subcmd[4] == 'n' && subcmd[5] == ' ') ||
            (subcmd[0] == 'b' && subcmd[1] == 'g' && subcmd[2] == ' ')) {
            kprint("Error: No se permite 'spawn' anidado.\n");
            uint32_t p = 0;
            fb_puts(out_buf, max_out, &p, "Error: spawn anidado no permitido.");
            return 1;
        }

        char th_name[32];
        th_name[0] = 'b'; th_name[1] = 'g'; th_name[2] = '_';
        uint32_t ni = 3;
        for (uint32_t i = 0; subcmd[i] && subcmd[i] != ' ' && ni < sizeof(th_name) - 1; ++i) {
            th_name[ni++] = subcmd[i];
        }
        th_name[ni] = '\0';

        struct bg_job *job = (struct bg_job *)kmalloc(sizeof(struct bg_job));
        if (!job) {
            kprint("Error: Sin memoria para nuevo trabajo.\n");
            uint32_t p = 0;
            fb_puts(out_buf, max_out, &p, "Error: sin memoria para spawn.");
            return 1;
        }

        uint32_t ci = 0;
        while (subcmd[ci] && ci < sizeof(job->cmd) - 1) {
            job->cmd[ci] = subcmd[ci];
            ci++;
        }
        job->cmd[ci] = '\0';

        struct tcb *new_t = thread_create(th_name, spawn_job_worker, job);
        if (!new_t) {
            kprint("Error: No se pudo instanciar el hilo.\n");
            kfree(job);
            uint32_t p = 0;
            fb_puts(out_buf, max_out, &p, "Error: fallo al crear hilo.");
            return 1;
        }

        job->tid = new_t->tid;

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
        kprint("' lanzada en segundo plano [TID ");
        kprint_dec(job->tid);
        kprint(", Hilo '"); kprint(th_name);
        kprint("'] -> Log: "); kprint(job->log_file); kprint("\n");

        uint32_t pos = 0;
        fb_puts(out_buf, max_out, &pos, "[SPAWN OK] TID=");
        fb_put_dec(out_buf, max_out, &pos, job->tid);
        fb_puts(out_buf, max_out, &pos, " Log=");
        fb_puts(out_buf, max_out, &pos, job->log_file);
        return 1;
    }

    if (cmd_line[0] == 'k' && cmd_line[1] == 'i' && cmd_line[2] == 'l' && cmd_line[3] == 'l' && cmd_line[4] == ' ') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        uint32_t target_tid = 0;
        while (*p >= '0' && *p <= '9') {
            target_tid = target_tid * 10 + (*p++ - '0');
        }
        if (thread_kill(target_tid)) {
            kprint("KTHREAD: Hilo TID ");
            kprint_dec(target_tid);
            kprint(" terminado con exito.\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, "Hilo terminado con exito.");
        } else {
            kprint("KTHREAD: Error al terminar TID ");
            kprint_dec(target_tid);
            kprint(" (no existe o es hilo del sistema 0..3).\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, "Error: TID invalido o hilo protegido.");
        }
        return 1;
    }

    if (cmd_line[0] == 'c' && cmd_line[1] == 'r' && cmd_line[2] == 'e' && cmd_line[3] == 'a' && cmd_line[4] == 'd' && cmd_line[5] == 'o' && cmd_line[6] == 'r') {
        kprint("SOMA (Sistema Operativo Multi-Agente) - Creado por Marcos\n");
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
        kprint("Durmiendo "); kprint_dec(ms); kprint(" ms (cooperativo)...\n");
        thread_sleep(ms);
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
        int code = http_get_host(tip, port, host_hdr, path, http_buf, sizeof(http_buf), &resp, 40000);

        uint32_t rlen = 0;
        while (http_buf[rlen] && rlen < sizeof(http_buf)) rlen++;
        int truncated = (rlen >= sizeof(http_buf) - 1);

        if (code >= 0) {
            kprint("\n--- RESPUESTA HTTP [Status "); kprint_dec((uint32_t)code); kprint("] ---\n");
            const char *display = (resp.body && resp.body[0] != '\0') ? resp.body : http_buf;
            kprint(display);
            if (truncated) {
                kprint("\n[TRUNCADO a 4096 B]");
            }
            kprint("\n-------------------------------------\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, display);
            if (truncated) {
                fb_puts(out_buf, max_out, &pos, "\n[TRUNCADO a 4096 B]");
            }
        } else if (http_buf[0] != '\0') {
            kprint("\n--- RESPUESTA RECIBIDA (AVISO: no es HTTP valido) ---\n");
            kprint(http_buf);
            if (truncated) {
                kprint("\n[TRUNCADO a 4096 B]");
            }
            kprint("\n----------------------------------------------------\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, "[AVISO: no es HTTP valido]\n");
            fb_puts(out_buf, max_out, &pos, http_buf);
            if (truncated) {
                fb_puts(out_buf, max_out, &pos, "\n[TRUNCADO a 4096 B]");
            }
        } else {
            kprint("Error en peticion HTTP (timeout o conexion cerrada).\n");
            uint32_t pos = 0;
            fb_puts(out_buf, max_out, &pos, "Error en peticion HTTP (timeout o conexion cerrada).");
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
        uint32_t i = 0; while (succ[i] && i < max_out - 1) { out_buf[i] = succ[i]; i++; } out_buf[i] = '\0';
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
        kheap_dump_stats();
        kheap_test_self();
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
        if (sec < SECTOR_USER_MIN || (sec >= 4095 && sec <= 4400)) {
            kprint("Error: sectores 0-2047 reservados al sistema y RamFS persistente.\n");
            const char *err = "Error: sector protegido (LBA < 2056 o 4095-4400 reservados). Usa LBA >= 2056.";
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
    } else if ((cmd_line[0] == 'f' && cmd_line[1] == 's' && cmd_line[2] == '-' && cmd_line[3] == 'b' && cmd_line[4] == 'a' && cmd_line[5] == 'c' && cmd_line[6] == 'k' && cmd_line[7] == 'u' && cmd_line[8] == 'p') ||
               (cmd_line[0] == 'c' && cmd_line[1] == 'h' && cmd_line[2] == 'e' && cmd_line[3] == 'c' && cmd_line[4] == 'k' && cmd_line[5] == 'p' && cmd_line[6] == 'o' && cmd_line[7] == 'i' && cmd_line[8] == 'n' && cmd_line[9] == 't')) {
        if (vfs_checkpoint_save() == 0) {
            kprint("[CHECKPOINT] Estado de RamFS y memoria persistido en virtio-blk (LBA 4096).\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Checkpoint guardado con exito.");
        } else {
            kprint("[CHECKPOINT ERROR] Fallo de I/O al guardar checkpoint en disco.\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Error al guardar checkpoint.");
        }
        return 1;
    } else if ((cmd_line[0] == 'f' && cmd_line[1] == 's' && cmd_line[2] == '-' && cmd_line[3] == 'r' && cmd_line[4] == 'e' && cmd_line[5] == 's' && cmd_line[6] == 't' && cmd_line[7] == 'o' && cmd_line[8] == 'r' && cmd_line[9] == 'e') ||
               (cmd_line[0] == 'r' && cmd_line[1] == 'o' && cmd_line[2] == 'l' && cmd_line[3] == 'l' && cmd_line[4] == 'b' && cmd_line[5] == 'a' && cmd_line[6] == 'c' && cmd_line[7] == 'k')) {
        if (vfs_checkpoint_restore() == 0) {
            kprint("[ROLLBACK] RamFS restaurado con exito desde el checkpoint de hardware (LBA 4096).\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "RamFS restaurado desde checkpoint.");
        } else {
            kprint("[ROLLBACK ERROR] No existe un checkpoint valido en LBA 4096 o fallo de I/O.\n");
            uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Error: checkpoint no encontrado.");
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
    const char *unrec = "Comando no implementado en el kernel o no reconocido.";
    uint32_t i = 0; while (unrec[i] && i < max_fb - 1) { feedback_out[i] = unrec[i]; i++; } feedback_out[i] = '\0';
    return 0;
}

static int is_valid_tool(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') s++;
    if (*s == '\0') return 0;

    /* Rechazar unicamente marcadores de plantilla '<...>' */
    for (const char *chk = s; *chk && *chk != '\n' && *chk != '\r'; chk++) {
        if (*chk == '<') return 0; /* Rechaza marcadores como <archivo>, pero permite '>' para redireccion */
    }

    return 1;
}


static int is_patch_target_protected(const char *name)
{
    static const char *const protected_list[] = {
        "boot.s", "idt.c", "idt.h", "io.h", "srcfs.c", "srcfs.h",
        "virtio_blk.c", "virtio_blk.h", "boot_gate.c", "boot_gate.h",
        "pmm.c", "pmm.h", "vmm.c", "vmm.h", "mem.c", "mem.h",
        "thread.c", "thread.h", "switch.s", "mutex.c", "mutex.h", 0
    };
    while (*name == ' ' || *name == '\t') name++;
    if (name[0] == '/') name++;
    if (name[0] == 's' && name[1] == 'r' && name[2] == 'c' && name[3] == '/') name += 4;

    for (int i = 0; protected_list[i]; ++i) {
        const char *a = name;
        const char *b = protected_list[i];
        while (*a && *b && (*a == *b)) { a++; b++; }
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\n' || *a == '\r' || *a == '"')) {
            return 1;
        }
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
    if (!hist_len || *hist_len >= sizeof(history_buf) - 400) {
        return;
    }
    uint32_t pos = *hist_len;

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

    /* Banner unificado en show_somafetch */

    for (;;) {
        int len;

        if (agent_resume_pending == 1) {
            /* Reanudar la mision tras un reinicio por parche. */
            const char *pre = "soma ";
            uint32_t ci = 0;
            while (*pre) cmd[ci++] = *pre++;
            for (uint32_t k = 0; agent_resume_mission[k] && ci < sizeof(cmd) - 1; ++k) {
                cmd[ci++] = agent_resume_mission[k];
            }
            cmd[ci] = '\0';
            len = (int)ci;
            serial_print("\033[1;36msoma\033[1;32m>\033[0m ");
            serial_print(cmd);
            serial_print("   [reanudacion automatica tras el reinicio]\n");
        } else {
            serial_print("\033[1;36msoma\033[1;32m>\033[0m ");
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
            serial_print("  help                 - Muestra esta ayuda\n  somafetch / neofetch - Ficha del sistema y telemetria en arte ASCII\n");
            serial_print("  creador              - Muestra informacion del creador de MYOS\n");
            serial_print("  test_suite           - Ejecuta la suite de auto-test y no-regresion\n");
            serial_print("  threads              - Estado de los hilos de kernel (Kthreads) y auto-test\n");
            serial_print("  ps                   - Lista de procesos y demonios activos con ticks CPU\n");
            serial_print("  spawn <cmd> / bg     - Lanza un comando como hilo en segundo plano\n");
            serial_print("  kill <tid>           - Termina un hilo en segundo plano (TID > 3)\n");
            serial_print("  date                 - Muestra la fecha y hora real (CMOS RTC)\n");
            serial_print("  time <cmd>           - Mide el tiempo y ciclos de CPU de un comando\n");
            serial_print("  free                 - Informacion de RAM fisica y KHeap\n");
            serial_print("  df                   - Informacion de almacenamiento y RamFS\n");
            serial_print("  tree                 - Muestra el arbol de directorios de RamFS\n");
            serial_print("  cp <orig> <dest>     - Copia un archivo en RamFS\n");
            serial_print("  mv <orig> <dest>     - Mueve o renombra un archivo en RamFS\n");
            serial_print("  touch <archivo>      - Crea un archivo vacio\n");
            serial_print("  head <arch> [lineas] - Muestra las primeras N lineas\n");
            serial_print("  tail <arch> [lineas] - Muestra las ultimas N lineas\n");
            serial_print("  wc <archivo>         - Cuenta lineas, palabras y bytes\n");
            serial_print("  echo <txt> [> f]     - Imprime texto o lo redirige a un archivo\n");
            serial_print("  <cmd> > <archivo>    - Redirige la salida de cualquier comando\n");
            serial_print("  hexdump <f> [bytes]  - Volcado hexadecimal y ASCII (alias: xxd)\n");
            serial_print("  reboot               - Reinicia la máquina físicamente (8042 reset)\n");
            serial_print("  poweroff             - Apaga el sistema limpiamente (ACPI)\n");
            serial_print("  halt                 - Sincroniza discos y detiene la CPU (HLT)\n");
            serial_print("  uptime               - Tiempo de ejecucion del kernel\n");
            serial_print("  sleep <ms>           - Suspende la CPU con HLT durante N milisegundos\n");
            serial_print("  dns <dominio>        - Consulta de registro A en servidor DNS\n");
            serial_print("  curl <host|ip> [pt]  - Peticion HTTP GET con resolucion DNS y cabecera Host\n");
            serial_print("  health               - Verifica estado del servidor LLM\n");
            serial_print("  llm <mensaje>        - Consulta general a nail-35b\n");
            serial_print("  llm-diag [pregunta]  - Telemetria + Diagnostico del kernel por IA\n");
            serial_print("  soma <mision>        - Agente autonomo IA (alias: myos, agent)\n");
            serial_print("  heap                 - Estado de la memoria dinamica kmalloc\n");
            serial_print("  ping <ip>            - Envia ICMP echo a una direccion IPv4\n");
            serial_print("  ls                   - Lista los archivos del RamFS\n");
            serial_print("  cat <archivo>        - Muestra el contenido de un archivo\n");
            serial_print("  write <arch> <texto> - Crea o sobrescribe un archivo\n");
            serial_print("  checkpoint           - Guarda snapshot fisico de RamFS en virtio-blk (alias: fs-backup)\n");
            serial_print("  rollback             - Restaura RamFS desde el ultimo checkpoint (alias: fs-restore)\n");
            serial_print("  fs-sync              - Fuerza sincronizacion de RamFS a virtio-blk\n");
            serial_print("  fs-format            - Restaura RamFS al estado inicial de fabrica\n");
            serial_print("  rm <archivo>         - Elimina un archivo\n");
            serial_print("  pmm                  - Estado y auto-test del gestor de frames fisicos (PMM)\n");
            serial_print("  vmm                  - Auto-test del gestor de memoria virtual (VMM 4 KiB)\n");
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
} else if ((cmd[0] == 's' && cmd[1] == 'o' && cmd[2] == 'm' && cmd[3] == 'a' && cmd[4] == ' ') ||
               (cmd[0] == 'm' && cmd[1] == 'y' && cmd[2] == 'o' && cmd[3] == 's' && cmd[4] == ' ') ||
               (cmd[0] == 'a' && cmd[1] == 'g' && cmd[2] == 'e' && cmd[3] == 'n' && cmd[4] == 't' && cmd[5] == ' ')) {
            const char *mission = (cmd[0] == 'a') ? (cmd + 6) : (cmd + 5);
            while (*mission == ' ') mission++;
            if (*mission == '\0') {
                serial_print("Uso: myos <mision en lenguaje natural>\n");
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
                const char *p_ctx = "'.\nContexto SOMA: Sistema Operativo Multi-Agente (kernel bare-metal x86_64). Red: IP local 10.0.2.15, Gateway 10.0.2.2. RamFS en /.\n"
                                    "Responde SIEMPRE con un objeto JSON valido con este esquema exacto:\n"
                                    "{\n"
                                    "  \"thought\": \"analisis breve de la accion a tomar\",\n"
                                    "  \"action\": \"tool\" | \"patch\" | \"final\",\n"
                                    "  \"cmd\": \"herramienta a ejecutar si action==tool\",\n"
                                    "  \"patch\": {\"file\": \"src/archivo.c\", \"search\": \"texto exacto existente, una sola vez\", \"replace\": \"texto nuevo\"} (con action==patch; cadenas vacias en otro caso),\n"
                                    "  \"verdict\": \"resumen completo y detallado para el usuario si action==final\"\n"
                                    "}\n"
                                    "Herramientas validas:\n"
                                    "- Diagnostico/Sistema: stats | mem | free (RAM libre/usada) | df (disco) | date (reloj real RTC) | time CMD | hexdump /arch | ps | spawn CMD | kill TID | reboot | poweroff | halt | arp | pci | ping 10.0.2.2 | uptime | sleep MS | test_suite\n"
                                    "- Red/Internet: dns DOMINIO | curl HOST [PUERTO] [RUTA]\n"
                                    "- Archivos/Memoria: ls | tree (arbol) | cat /arch | grep PATRON /arch | head /arch N | tail /arch N | wc /arch | cp /orig /dest | mv /orig /dest | touch /arch | write /arch texto | echo texto [> /arch] | CMD > /arch (redireccion universal) | rm /arch | fs-sync | fs-format\n"
                                    "- Disco/Sectores: sector_read LBA | sector_write LBA texto\n"
                                    "- Codigo Fuente: src_ls | src_cat archivo.c OFFSET | src_grep archivo.c texto\n"
                                    "Reglas de oro:\n"
                                    "1) Si la mision pide varias tareas (ej: resolver DNS y hacer curl), ejecuta UNA herramienta por paso hasta completar TODAS.\n"
                                    "2) Para verificar el estado general del kernel ejecuta 'test_suite'.\n"
                                    "3) En 'verdict' explica con claridad y detalle todo lo realizado. NUNCA uses respuestas vacias ni '...'.\n"
                                    "4) Modificacion de codigo: Los subsistemas del core (boot, idt, pmm, vmm, mem, thread, mutex, virtio_blk, boot_gate) estan PROTEGIDOS. Solo puedes modificar archivos como kernel.c, console.c, fs.c, etc. El bloque SEARCH debe ser conciso y exacto (max 120 lineas). Con action==patch el kernel se reiniciara y validara el parche.\n"
                                    "5) Si el historial indica que tu parche fue aprobado (BOOT_OK), NO envies mas parches: comprueba el archivo y concluye con action=\"final\".\n"
                                    "6) AUTO-EVOLUCION Y MEMORIA: Si creas o modificas una herramienta o comando, anota SIEMPRE con 'write' en /etc/mem_kernel.txt el nombre del comando y que hace para recordarlo en futuras misiones.\n"
                                    "7) PERSISTENCIA: Si la mision pide guardar datos, debes asegurarte de que se escriban en disco con 'write /archivo texto', 'echo texto > /archivo' o 'CMD > /archivo'. Si ya ejecutaste con exito una redireccion '>' o un 'write', el archivo YA esta persistido en disco y puedes concluir directamente con action=\"final\".\n"
                                    "8) TAREAS EN SEGUNDO PLANO: Para tareas demoradas puedes usar 'spawn <cmd>' (ej: 'spawn ping 10.0.2.2' o 'spawn sleep 3000'), monitorearlas con 'ps' y leer su resultado en '/tmp/job_<TID>.log'.\n"
                                    "9) LECTURA EFICIENTE: Si la mision pide ver el inicio o final de un archivo grande o un log, usa 'head /archivo N' o 'tail /archivo N' en vez de volcar todo con 'cat'. Para explorar la jerarquia usa 'tree'.\n";
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

                const char *p_rules = "\nInstruccion: Analiza el historial. Si la mision requeria guardar o escribir un archivo y aun no has ejecutado write, debes ejecutar la herramienta write ahora antes de emitir tu dictamen final. Genera el JSON.";
                while (*p_rules && ap_len < sizeof(agent_prompt_buf) - 1) agent_prompt_buf[ap_len++] = *p_rules++;
                agent_prompt_buf[ap_len] = '\0';

                if (!llm_chat_json(agent_prompt_buf, agent_reply_buf, sizeof(agent_reply_buf), 40000)) {
                    serial_print("Error en la comunicacion estructurada con el agente.\n");
                    finished = 1;
                    break;
                }

                static char act[32];
                static char th[1024];
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
                        const char *target_fname = pfile + 10;
                        if (is_patch_target_protected(target_fname)) {
                            serial_print("\n[SEGURIDAD KERNEL] Modificacion denegada: Archivo del core protegido.\n");
                            history_add_note(&hist_len, step,
                                "tu parche fue rechazado por seguridad: los subsistemas del core "
                                "(boot, idt, pmm, vmm, mem, thread, mutex, virtio_blk, boot_gate) estan PROTEGIDOS. "
                                "Solo puedes modificar utilidades de aplicacion (ej: kernel.c, console.c, fs.c, etc.).");
                            continue;
                        }
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

                        tool_feedback_buf[0] = '\0';
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

                /* 3. Accion: Dictamen final con Guardia Cognitiva Anti-Alucinacion */
                if (find_substr(act, "final") || verdict[0] != '\0') {
                    /* Verificar si la mision pedia crear/guardar archivos y el agente intento fingir el guardado */
                    int req_file = (find_substr(mission, "crea") != 0 ||
                                    find_substr(mission, "guarda") != 0 ||
                                    find_substr(mission, "escribe") != 0 ||
                                    find_substr(mission, "archivo") != 0 ||
                                    find_substr(mission, ".txt") != 0 ||
                                    find_substr(mission, ".log") != 0);

                    int file_saved = (find_substr(history_buf, "ejecutaste 'write ") != 0 ||
                                      find_substr(history_buf, "ejecutaste 'echo ") != 0 ||
                                      find_substr(history_buf, " > ") != 0);

                    if (req_file && !file_saved && step < 7) {
                        serial_print("\n>> [GUARDIA SOMA]: Bloqueada alucinacion de guardado mental.\n");
                        serial_print(">> [SOMA KERNEL]: No has ejecutado la herramienta 'write' en el hardware.\n");
                        history_add_note(&hist_len, step,
                            "RECHAZADO: Declaraste haber completado la mision pero NO ejecutaste la herramienta 'write'. "
                            "NO hagas guardados mentales. Genera OBLIGATORIAMENTE un JSON con action=\"tool\" y "
                            "cmd=\"write /archivo contenido\" para persistirlo en disco.");
                        continue;
                    }

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
            serial_print("ARCH: "); serial_print(MYOS_ARCH_NAME);
            serial_print(" (HAL v1) | IP: "); kprint_ip(net_ip);
            serial_print(" | GW: "); kprint_ip(net_gateway);
            serial_print(" | MAC: "); kprint_mac(net_mac);
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
    pmm_init(multiboot_magic, multiboot_info_addr);
    vmm_init();
    vmm_apply_protections();
    kheap_init();
    thread_init();
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

    /* Comprobacion ultrarrapida en modo Canary (<1s) antes de pruebas de red */
    boot_gate_check();

    net_run_llm_test();

    clear_screen();

    /* Portada visual estandar de SOMA (70 columnas sin overflow) */
    static char dummy_fb[64];
    show_somafetch(dummy_fb, sizeof(dummy_fb));

    kprint("\033[1;36m----------------------------------------------------------------------\033[0m\n");
    kprint(" \033[1;37mSOMA 0.2 :: Escribe '\033[1;32msoma <mision>\033[1;37m' para el Agente o '\033[1;33mhelp\033[1;37m'.\033[0m\n");
    kprint("\033[1;36m----------------------------------------------------------------------\033[0m\n\n");

    agent_state_load();

    shell_run();
}
