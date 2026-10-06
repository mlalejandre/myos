#ifndef SOMA_RTC_H
#define SOMA_RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
};

void rtc_get_datetime(struct rtc_time *t);

#endif
