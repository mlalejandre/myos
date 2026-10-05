#include "thread.h"
#include "console.h"
#include "io.h"

#define COM1 0x3F8

static volatile uint16_t *const VGA_BUF = (uint16_t *)0xB8000;
static uint32_t vga_row = 0;
static uint32_t vga_col = 0;

static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(vga_row * 80 + vga_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void console_clear(void)
{
    for (uint32_t i = 0; i < 80 * 25; ++i) {
        VGA_BUF[i] = 0x0720;
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

static void vga_scroll(void)
{
    while (vga_row >= 25) {
        for (int i = 0; i < 24 * 80; ++i) {
            VGA_BUF[i] = VGA_BUF[i + 80];
        }
        for (int i = 24 * 80; i < 25 * 80; ++i) {
            VGA_BUF[i] = 0x0720;
        }
        vga_row--;
    }
}

static void vga_putc(char c)
{
    if (c == '\r') {
        vga_col = 0;
    } else if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUF[vga_row * 80 + vga_col] = 0x0720;
        }
    } else if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
        VGA_BUF[vga_row * 80 + vga_col] = (uint16_t)(0x0700 | (uint8_t)c);
        vga_col++;
        if (vga_col >= 80) {
            vga_col = 0;
            vga_row++;
        }
    }
    vga_scroll();
    vga_update_cursor();
}

void kputc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);

    vga_putc(c);
}

void kprint(const char *s)
{
    for (; *s; ++s) {
        kputc(*s);
    }
}

static void put_nibble(uint8_t v)
{
    v &= 0x0F;
    kputc(v < 10 ? (char)('0' + v) : (char)('A' + v - 10));
}

void kprint_hex8(uint8_t value)
{
    put_nibble((uint8_t)(value >> 4));
    put_nibble(value);
}

void kprint_hex16(uint16_t value)
{
    kprint_hex8((uint8_t)(value >> 8));
    kprint_hex8((uint8_t)value);
}

void kprint_hex32(uint32_t value)
{
    kprint_hex16((uint16_t)(value >> 16));
    kprint_hex16((uint16_t)value);
}

void kprint_dec(uint32_t value)
{
    char tmp[10];
    int n = 0;

    if (value == 0) {
        kputc('0');
        return;
    }

    while (value != 0) {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0) {
        kputc(tmp[--n]);
    }
}

void kprint_mac(const uint8_t *mac)
{
    for (int i = 0; i < 6; ++i) {
        kprint_hex8(mac[i]);
        if (i != 5) kputc(':');
    }
}

void kprint_ip(const uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        kprint_dec(ip[i]);
        if (i != 3) kputc('.');
    }
}

/* ====================================================================
 * Controlador de Teclado PS/2 (Scan Code Set 1 - IRQ1)
 * ==================================================================== */
#define KBD_BUF_SIZE 128
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;
static int shift_active = 0;
static int caps_active = 0;

static const char kbd_us_lower[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0, /* Ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* LShift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, /* RShift */
    '*', 0, /* Alt */ ' ', 0 /* CapsLock */
};

static const char kbd_us_upper[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0, /* Ctrl */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, /* LShift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, /* RShift */
    '*', 0, /* Alt */ ' ', 0 /* CapsLock */
};

void keyboard_irq_handler(void)
{
    uint8_t sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) {
        shift_active = 1;
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift_active = 0;
        return;
    }
    if (sc == 0x3A) {
        caps_active = !caps_active;
        return;
    }
    if (sc & 0x80) {
        return; /* Ignorar key release */
    }
    if (sc < sizeof(kbd_us_lower)) {
        char ch = shift_active ? kbd_us_upper[sc] : kbd_us_lower[sc];
        if (caps_active) {
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        }
        if (ch != 0) {
            uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buf[kbd_head] = ch;
                kbd_head = next;
            }
        }
    }
}

int kgetc_ready(void)
{
    if (kbd_head != kbd_tail) {
        return 1;
    }
    return (inb(COM1 + 5) & 0x01) != 0;
}

char kgetc(void)
{
    while (!kgetc_ready()) {
        thread_yield();
        __asm__ volatile ("sti; hlt");   /* el tick de 1 kHz nos despierta */
    }
    if (kbd_head != kbd_tail) {
        char ch = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        return ch;
    }
    return (char)inb(COM1);
}

int kgetline(char *buf, uint32_t max)
{
    if (!buf || max == 0) {
        return 0;
    }

    uint32_t i = 0;

    while (i < max - 1) {
        char c = kgetc();

        if (c == '\r' || c == '\n') {
            kputc('\r');
            kputc('\n');
            break;
        }

        if (c == '\b' || c == 0x7F) {
            if (i > 0) {
                --i;
                kprint("\b \b");
            }
            continue;
        }

        if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
            buf[i++] = c;
            kputc(c);
        }
    }

    buf[i] = '\0';
    return (int)i;
}
