#include <stdint.h>

#include "console.h"
#include "net.h"
#include "mem.h"
#include "sysinfo.h"
#include "virtio_net.h"
#include "io.h"

static inline uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_rsp(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(val));
    return val;
}

static int append_str(char *dst, uint32_t max, uint32_t *pos, const char *src)
{
    while (*src && *pos < max - 1) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
    return (*pos < max - 1);
}

static int append_dec(char *dst, uint32_t max, uint32_t *pos, uint64_t val)
{
    char tmp[24];
    int n = 0;
    if (val == 0) {
        return append_str(dst, max, pos, "0");
    }
    while (val > 0) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (n > 0 && *pos < max - 1) {
        dst[(*pos)++] = tmp[--n];
    }
    dst[*pos] = '\0';
    return (*pos < max - 1);
}

static int append_hex(char *dst, uint32_t max, uint32_t *pos, uint64_t val, int digits)
{
    append_str(dst, max, pos, "0x");
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((val >> shift) & 0x0F);
        char ch = nibble < 10 ? (char)('0' + nibble) : (char)('A' + nibble - 10);
        if (*pos < max - 1) {
            dst[(*pos)++] = ch;
        }
    }
    dst[*pos] = '\0';
    return (*pos < max - 1);
}

static int append_ip(char *dst, uint32_t max, uint32_t *pos, const uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        append_dec(dst, max, pos, ip[i]);
        if (i < 3) append_str(dst, max, pos, ".");
    }
    return (*pos < max - 1);
}


/* ---- Calibracion PIT Canal 2 vs TSC ------------------------------ */
uint64_t tsc_ticks_per_ms = 1000000ULL; /* Valor inicial de seguridad */
uint64_t tsc_freq_mhz     = 1000ULL;

uint64_t timer_calibrate_tsc(void)
{
    /* Desactivar altavoz/gate del canal 2 */
    outb(0x61, (uint8_t)(inb(0x61) & ~0x03));

    /* Canal 2, acceso LSB luego MSB, Modo 0 (one-shot), binario */
    outb(0x43, 0xB0);

    /* 11932 cuentas = 10.0001 ms contra 1.193182 MHz */
    uint16_t count = 11932;
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    /* Iniciar conteo activando gate (bit 0) */
    uint8_t orig = inb(0x61);
    outb(0x61, (uint8_t)((orig & ~0x02) | 0x01));

    uint64_t start = rdtsc();

    /* Esperar a que el bit 5 pase a 1 (conteo terminado tras 10 ms) */
    while ((inb(0x61) & 0x20) == 0) {
        cpu_pause();
    }

    uint64_t end = rdtsc();

    /* Desactivar gate */
    outb(0x61, (uint8_t)(inb(0x61) & ~0x01));

    uint64_t delta = end - start;
    if (delta > 500000ULL && delta < 1000000000ULL) {
        tsc_ticks_per_ms = delta / 10ULL;
        tsc_freq_mhz     = tsc_ticks_per_ms / 1000ULL;
    } else {
        tsc_ticks_per_ms = 1000000ULL;
        tsc_freq_mhz     = 1000ULL;
    }

    return tsc_ticks_per_ms;
}

void sysinfo_get(struct sysinfo *info)
{
    info->cr0 = read_cr0();
    info->cr3 = read_cr3();
    info->cr4 = read_cr4();
    info->rsp = read_rsp();

    virtio_net_stats(&info->tx_packets, &info->rx_packets, &info->tx_bytes, &info->rx_bytes);

    info->mac = virtio_net_mac();
    info->ip  = net_ip;
    info->gw  = net_gateway;
}

void sysinfo_print_mem(void)
{
    struct sysinfo s;
    sysinfo_get(&s);

    kprint("\nESTADO DE MEMORIA Y CPU:\n");
    kprint("------------------------\n");
    kprint("Long Mode:       ACTIVO (x86_64)\n");
    kprint("CR3 (PML4 base): 0x"); kprint_hex32((uint32_t)s.cr3); kprint("\n");
    kprint("CR0:             0x"); kprint_hex32((uint32_t)s.cr0); kprint("\n");
    kprint("CR4 (PAE/OSFXSR):0x"); kprint_hex32((uint32_t)s.cr4); kprint("\n");
    kprint("Pila actual RSP: 0x"); kprint_hex32((uint32_t)s.rsp); kprint("\n");
    kprint("Virtqueue 0 (RX):0x00200000 (fija en 2 MiB)\n");
    kprint("Mapeo inicial:   0x00000000 - 0x40000000 (1 GiB identity-mapped)\n\n");
}

void sysinfo_print_stats(void)
{
    struct sysinfo s;
    sysinfo_get(&s);

    kprint("\nESTADISTICAS DE RED (VirtIO-NET):\n");
    kprint("---------------------------------\n");
    kprint("Paquetes TX enviados:   "); kprint_dec(s.tx_packets); kprint("\n");
    kprint("Bytes TX enviados:      "); kprint_dec((uint32_t)s.tx_bytes); kprint(" B\n");
    kprint("Paquetes RX recibidos:  "); kprint_dec(s.rx_packets); kprint("\n");
    kprint("Bytes RX recibidos:     "); kprint_dec((uint32_t)s.rx_bytes); kprint(" B\n\n");
}

int sysinfo_format_telemetry(char *out_buf, uint32_t max)
{
    struct sysinfo s;
    sysinfo_get(&s);

    uint32_t pos = 0;
    append_str(out_buf, max, &pos, "[TELEMETRIA KERNEL MYOS x86_64]\n");
    append_str(out_buf, max, &pos, "- CPU: 64-bit Long Mode | CR0=");
    append_hex(out_buf, max, &pos, s.cr0, 8);
    append_str(out_buf, max, &pos, " | CR4=");
    append_hex(out_buf, max, &pos, s.cr4, 8);
    append_str(out_buf, max, &pos, "\n- Paginacion: PML4 CR3=");
    append_hex(out_buf, max, &pos, s.cr3, 8);
    append_str(out_buf, max, &pos, " (1 GiB mapeado)\n- Pila actual: RSP=");
    append_hex(out_buf, max, &pos, s.rsp, 8);
    append_str(out_buf, max, &pos, "\n- VirtIO-NET: IP=");
    append_ip(out_buf, max, &pos, s.ip);
    append_str(out_buf, max, &pos, " GW=");
    append_ip(out_buf, max, &pos, s.gw);
    append_str(out_buf, max, &pos, " | TX_pkts=");
    append_dec(out_buf, max, &pos, s.tx_packets);
    append_str(out_buf, max, &pos, " RX_pkts=");
    append_dec(out_buf, max, &pos, s.rx_packets);
    append_str(out_buf, max, &pos, "\n");

    return (int)pos;
}

int sysinfo_format_mem(char *out_buf, uint32_t max)
{
    struct sysinfo s;
    sysinfo_get(&s);

    size_t used = 0;
    size_t free_b = 0;
    kheap_stats(&used, &free_b);

    uint32_t pos = 0;
    append_str(out_buf, max, &pos, "ESTADO MEMORIA/CPU: x86_64 Long Mode (1 GiB identity-mapped)\n");
    append_str(out_buf, max, &pos, "- CR0=");
    append_hex(out_buf, max, &pos, s.cr0, 8);
    append_str(out_buf, max, &pos, " | CR3(PML4)=");
    append_hex(out_buf, max, &pos, s.cr3, 8);
    append_str(out_buf, max, &pos, " | CR4=");
    append_hex(out_buf, max, &pos, s.cr4, 8);
    append_str(out_buf, max, &pos, " | RSP=");
    append_hex(out_buf, max, &pos, s.rsp, 8);
    append_str(out_buf, max, &pos, "\n- Heap kmalloc: ");
    append_dec(out_buf, max, &pos, (uint64_t)used);
    append_str(out_buf, max, &pos, " bytes usados, ");
    append_dec(out_buf, max, &pos, (uint64_t)(free_b / 1024));
    append_str(out_buf, max, &pos, " KiB libres (total 12 MiB en 0x00400000)\n");
    append_str(out_buf, max, &pos, "- Virtqueue 0 (RX): 0x00200000 (2 MiB fija)");

    return (int)pos;
}
