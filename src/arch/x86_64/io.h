#ifndef MYOS_IO_H
#define MYOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void mem_barrier(void)
{
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void cpu_pause(void)
{
    __asm__ volatile ("pause");
}

/*
 * Salida limpia de QEMU mediante el dispositivo isa-debug-exit
 * (-device isa-debug-exit,iobase=0xf4,iosize=0x04).
 * El proceso QEMU termina con codigo (code << 1) | 1.
 */
static inline void qemu_exit(uint8_t code)
{
    outb(0xF4, code);
}

#endif
