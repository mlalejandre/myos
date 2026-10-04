#ifndef MYOS_SYSINFO_H
#define MYOS_SYSINFO_H

#include <stdint.h>

struct sysinfo {
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t rsp;

    uint32_t tx_packets;
    uint32_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;

    const uint8_t *mac;
    const uint8_t *ip;
    const uint8_t *gw;
};

void sysinfo_get(struct sysinfo *info);
void sysinfo_print_mem(void);
void sysinfo_print_stats(void);

/* Genera un informe compacto en formato texto dentro de out_buf */
int sysinfo_format_telemetry(char *out_buf, uint32_t max);
/* Genera un informe veraz de CPU, registros y heap para el agente/shell */
int sysinfo_format_mem(char *out_buf, uint32_t max);

#endif
