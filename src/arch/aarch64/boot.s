.section .text.boot
.global _start
_start:
    /* Leer ID del core y detener núcleos secundarios */
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbz x0, master
hang:
    wfe
    b hang

master:
    /* 1. Inicializar Stack Pointer (512 KiB bajo el kernel) */
    ldr x1, =0x40080000
    mov sp, x1

    /* 2. Configurar la tabla de vectores de excepciones VBAR_EL1 */
    ldr x0, =vectors
    msr vbar_el1, x0
    isb

    /* 3. Limpiar la sección .bss completa a cero (16 bytes por paso) */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
    sub x2, x1, x0
    cbz x2, bss_done
bss_loop:
    stp xzr, xzr, [x0], #16
    cmp x0, x1
    b.lo bss_loop
bss_done:

    bl kernel_main
    b hang

/* ====================================================================
 * TABLA VECTORIAL DE EXCEPCIONES AARCH64 (VBAR_EL1)
 * ==================================================================== */
.align 11
.global vectors
vectors:
    /* Current EL with SP0 */
    .align 7
    b exc_entry_curr_sp0_sync
    .align 7
    b exc_entry_curr_sp0_irq
    .align 7
    b exc_entry_curr_sp0_fiq
    .align 7
    b exc_entry_curr_sp0_serror

    /* Current EL with SPx */
    .align 7
    b exc_entry_curr_spx_sync
    .align 7
    b exc_entry_curr_spx_irq
    .align 7
    b exc_entry_curr_spx_fiq
    .align 7
    b exc_entry_curr_spx_serror

    /* Lower EL using AArch64 */
    .align 7
    b exc_entry_lower_a64_sync
    .align 7
    b exc_entry_lower_a64_irq
    .align 7
    b exc_entry_lower_a64_fiq
    .align 7
    b exc_entry_lower_a64_serror

    /* Lower EL using AArch32 */
    .align 7
    b exc_entry_lower_a32_sync
    .align 7
    b exc_entry_lower_a32_irq
    .align 7
    b exc_entry_lower_a32_fiq
    .align 7
    b exc_entry_lower_a32_serror

.macro EXC_STUB name, type
\name:
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    mov x0, #\type
    mov x1, sp
    bl arm64_exception_handler

    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    eret
.endm

EXC_STUB exc_entry_curr_sp0_sync, 0
EXC_STUB exc_entry_curr_sp0_irq, 1
EXC_STUB exc_entry_curr_sp0_fiq, 2
EXC_STUB exc_entry_curr_sp0_serror, 3

EXC_STUB exc_entry_curr_spx_sync, 4
EXC_STUB exc_entry_curr_spx_irq, 5
EXC_STUB exc_entry_curr_spx_fiq, 6
EXC_STUB exc_entry_curr_spx_serror, 7

EXC_STUB exc_entry_lower_a64_sync, 8
EXC_STUB exc_entry_lower_a64_irq, 9
EXC_STUB exc_entry_lower_a64_fiq, 10
EXC_STUB exc_entry_lower_a64_serror, 11

EXC_STUB exc_entry_lower_a32_sync, 12
EXC_STUB exc_entry_lower_a32_irq, 13
EXC_STUB exc_entry_lower_a32_fiq, 14
EXC_STUB exc_entry_lower_a32_serror, 15

.section .data
.align 8
.global multiboot_magic
.global multiboot_info_addr
multiboot_magic:
    .long 0
multiboot_info_addr:
    .long 0
