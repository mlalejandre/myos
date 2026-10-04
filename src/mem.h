/* nota de la IA */
#ifndef MYOS_MEM_H
#define MYOS_MEM_H
#include <stddef.h>


void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

void  kheap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  kheap_stats(size_t *used, size_t *free_bytes);

#endif
