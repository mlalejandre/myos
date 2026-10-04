#ifndef MYOS_CONSOLE_H
#define MYOS_CONSOLE_H
/* hpatch ok */

#include <stdint.h>

/* Requiere que el puerto serie ya este inicializado (kernel.c). */

void kputc(char c);
void kprint(const char *s);
void kprint_hex8(uint8_t value);
void kprint_hex16(uint16_t value);
void kprint_hex32(uint32_t value);
void kprint_dec(uint32_t value);
void kprint_mac(const uint8_t *mac);
void kprint_ip(const uint8_t *ip);

int  kgetc_ready(void);
char kgetc(void);
int  kgetline(char *buf, uint32_t max);

#endif
