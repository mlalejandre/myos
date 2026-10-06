.section .text
.global switch_context
.type switch_context, %function
switch_context:
    /* Guardar registros callee-saved (x19-x30) */
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    mov x2, sp
    str x2, [x0]
    /* Restaurar el SP del nuevo hilo */
    mov sp, x1
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret

.global thread_trampoline_asm
.type thread_trampoline_asm, %function
thread_trampoline_asm:
    bl thread_trampoline_c
    bl thread_exit
    b .
