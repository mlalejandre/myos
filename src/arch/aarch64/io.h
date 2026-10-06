#ifndef MYOS_IO_H
#define MYOS_IO_H
#include <stdint.h>
static inline void outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
static inline void outw(uint16_t port, uint16_t value) { (void)port; (void)value; }
static inline void outl(uint16_t port, uint32_t value) { (void)port; (void)value; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }
static inline void io_wait(void) {}
static inline void cpu_pause(void) { __asm__ volatile("yield"); }
static inline void mem_barrier(void) { __asm__ volatile("dmb ish"); }
static inline uint64_t rdtsc(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}
/* PARCHE 046: salida de QEMU por semihosting (SYS_EXIT). Requiere '-semihosting'.
 * El estado se codifica (code << 1) | 1, identico a isa-debug-exit de x86_64, para que
 * ejecutar.py reutilice las mismas constantes (0x41 gate OK, 0x45 fallo, 0x47 panic). */
static inline void qemu_exit(uint8_t code)
{
    volatile uint64_t blk[2] __attribute__((aligned(16)));
    blk[0] = 0x20026ULL; /* ADP_Stopped_ApplicationExit */
    blk[1] = (uint64_t)(((uint32_t)code << 1) | 1u);
    register uint64_t x0 __asm__("x0") = 0x18; /* SYS_EXIT */
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)blk;
    __asm__ volatile ("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
}
#endif
