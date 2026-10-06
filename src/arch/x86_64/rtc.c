#include "rtc.h"
#include "io.h"
#include "arch.h"

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

void rtc_get_datetime(struct rtc_time *t)
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
