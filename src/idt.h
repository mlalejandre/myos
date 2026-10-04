#ifndef MYOS_IDT_H
#define MYOS_IDT_H

#include <stdint.h>

struct trap_frame {
    /* Registros de proposito general (guardados por isr_common_stub) */
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    /* Empujados por el stub especifico */
    uint64_t vector;
    uint64_t error_code;

    /* Empujados automaticamente por la CPU x86_64 */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Inicializa la IDT y carga el registro IDTR */
void idt_init(void);

extern volatile int boot_gate_active;

#endif

/* Temporizador del sistema y gestion de IRQ */
extern volatile uint64_t timer_ticks;
uint64_t timer_get_uptime_ms(void);
void timer_sleep_ms(uint32_t ms);
void pic_remap(void);
void timer_init(uint32_t freq_hz);
