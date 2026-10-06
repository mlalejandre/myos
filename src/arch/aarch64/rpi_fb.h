#ifndef SOMA_RPI_FB_H
#define SOMA_RPI_FB_H

#include <stdint.h>

int  rpi_fb_init(void);
void rpi_fb_putc(char c);
void rpi_fb_clear(void);

#endif
