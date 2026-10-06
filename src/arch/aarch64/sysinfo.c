#include "sysinfo.h"

uint64_t tsc_ticks_per_ms = 1000;
uint64_t tsc_freq_mhz = 62;

uint64_t timer_calibrate_tsc(void)
{
    uint64_t freq = 62500000;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    tsc_ticks_per_ms = freq / 1000;
    tsc_freq_mhz = freq / 1000000;
    return tsc_ticks_per_ms;
}

void sysinfo_get(struct sysinfo *info)
{
    if (!info) return;
    info->cr0 = 0;
    info->cr3 = 0;
    info->cr4 = 0;
    __asm__ volatile ("mov %0, sp" : "=r"(info->rsp));
    info->tx_packets = 0;
    info->rx_packets = 0;
    info->tx_bytes = 0;
    info->rx_bytes = 0;
    info->mac = 0;
    info->ip = 0;
    info->gw = 0;
}

void sysinfo_print_mem(void) {}
void sysinfo_print_stats(void) {}
int sysinfo_format_telemetry(char *out_buf, uint32_t max) { (void)out_buf; (void)max; return 0; }
int sysinfo_format_mem(char *out_buf, uint32_t max) { (void)out_buf; (void)max; return 0; }
