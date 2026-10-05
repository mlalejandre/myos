.section .text
.code64
.global switch_context
.type switch_context, @function

/*
 * switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 *   %rdi = direccion en memoria donde guardar el RSP actual
 *   %rsi = valor del nuevo RSP a cargar
 */
switch_context:
    /* 1. Guardar registros callee-saved y banderas de CPU */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq

    /* 2. Guardar RSP saliente */
    movq %rsp, (%rdi)

    /* 3. Conmutar a la pila del nuevo hilo */
    movq %rsi, %rsp

    /* 4. Restaurar registros del hilo entrante */
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    /* 5. Saltar a la instruccion de retorno o trampolin */
    ret

.global thread_trampoline_asm
.type thread_trampoline_asm, @function
thread_trampoline_asm:
    sti
    /*
     * Al entrar aqui tras ret, %rsp es multiplo de 16.
     * Restamos 8 bytes para simular que entramos mediante 'call',
     * garantizando que dentro de la funcion C (%rsp + 8) % 16 == 0 (ABI System V).
     */
    subq $8, %rsp
    call thread_trampoline_c
    addq $8, %rsp
    call thread_exit
    hlt

.section .note.GNU-stack,"",@progbits
