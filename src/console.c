#include "console.h"
#include "io.h"

#define COM1 0x3F8

void kputc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }

    outb(COM1, (uint8_t)c);
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

int kgetc_ready(void)
{
    return (inb(COM1 + 5) & 0x01) != 0;
}

char kgetc(void)
{
    while (!kgetc_ready()) {
        cpu_pause();
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
