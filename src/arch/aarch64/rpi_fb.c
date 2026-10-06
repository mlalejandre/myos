#include "rpi_fb.h"
#include "arch.h"
#include "console.h"

int rpi_fb_init(void)
{
#ifndef PLATFORM_RPI4
    /* En QEMU 'virt' no existe el Mailbox de Broadcom (evita External Abort) */
    return 0;
#else
    #define RPI_MAILBOX_BASE 0xFE00B880ULL
    #define MAILBOX_READ     ((volatile uint32_t *)(RPI_MAILBOX_BASE + 0x00))
    #define MAILBOX_STATUS   ((volatile uint32_t *)(RPI_MAILBOX_BASE + 0x18))
    #define MAILBOX_WRITE    ((volatile uint32_t *)(RPI_MAILBOX_BASE + 0x20))
    #define MAILBOX_FULL     (1 << 31)
    #define MAILBOX_EMPTY    (1 << 30)
    #define MAILBOX_CH_PROP  8

    __attribute__((aligned(16))) static volatile uint32_t mailbox_buf[36] = {
        36 * 4, 0,
        0x00040003, 8, 0, 1024, 768,
        0x00040004, 8, 0, 1024, 768,
        0x00040005, 4, 0, 32,
        0x00040001, 8, 0, 16, 0,
        0x00000000
    };

    /* (Lógica de inicialización Mailbox RPi 4...) */
    return 0;
#endif
}

void rpi_fb_clear(void) {}
void rpi_fb_putc(char c) { (void)c; }
