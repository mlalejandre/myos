#include "rtc.h"

void rtc_get_datetime(struct rtc_time *t)
{
    if (!t) return;
    volatile uint32_t *pl031_dr = (volatile uint32_t *)0x09010000;
    uint32_t epoch = *pl031_dr;

    if (epoch == 0) {
        t->year = 2026; t->month = 10; t->day = 6;
        t->hour = 20; t->min = 0; t->sec = 0;
        return;
    }

    t->sec = (uint8_t)(epoch % 60);
    epoch /= 60;
    t->min = (uint8_t)(epoch % 60);
    epoch /= 60;
    t->hour = (uint8_t)(epoch % 24);
    epoch /= 24;

    uint32_t days = epoch;
    uint32_t year = 1970;
    while (1) {
        uint32_t leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t dim = leap ? 366 : 365;
        if (days < dim) break;
        days -= dim;
        year++;
    }
    t->year = (uint16_t)year;

    uint32_t leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    static const uint8_t days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint8_t month = 1;
    for (int m = 0; m < 12; ++m) {
        uint8_t dim = days_in_month[m];
        if (m == 1 && leap) dim = 29;
        if (days < dim) break;
        days -= dim;
        month++;
    }
    t->month = month;
    t->day = (uint8_t)(days + 1);
}
