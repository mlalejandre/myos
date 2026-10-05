#ifndef MYOS_ARCH_H
#define MYOS_ARCH_H

#include <stdint.h>

#define MYOS_ARCH_NAME "x86_64"
#define MYOS_ARCH_BITS 64

/* ====================================================================
 * PRIMITIVAS DE CPU Y BARRERAS DE MEMORIA
 * ==================================================================== */

static inline void arch_pause(void)
{
    __asm__ volatile ("pause");
}

static inline void arch_halt(void)
{
    __asm__ volatile ("hlt");
}

static inline void arch_interrupts_enable(void)
{
    __asm__ volatile ("sti");
}

static inline void arch_interrupts_disable(void)
{
    __asm__ volatile ("cli");
}

static inline void arch_memory_barrier(void)
{
    __asm__ volatile ("mfence" ::: "memory");
}

static inline uint64_t arch_cycle_counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif
