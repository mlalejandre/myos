.section .multiboot
.align 4
.long 0x1BADB002
.long 0x00000000
.long -(0x1BADB002 + 0x00000000)

.section .text
.code32

.global _start
.extern kernel_main

_start:
    cli

    movl $stack_top, %esp

    call setup_long_mode

    lgdt gdt64_desc
    ljmp $0x08, $long_mode_start


setup_long_mode:

    movl $pml4_table, %edi
    xorl %eax, %eax
    movl $512, %ecx
    rep stosl

    movl $pdpt_table, %edi
    xorl %eax, %eax
    movl $512, %ecx
    rep stosl

    movl $pd_table, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    movl $pdpt_table, %eax
    orl $0x3, %eax
    movl %eax, pml4_table

    movl $pd_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table

    movl $pd_table, %edi
    xorl %ebx, %ebx
    movl $512, %ecx

map_pd:
    movl %ebx, %eax
    orl $0x83, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)

    addl $0x200000, %ebx
    addl $8, %edi

    loop map_pd

    /*
     * Enable PAE and OSFXSR.
     *
     * PAE:
     *   CR4 bit 5
     *
     * OSFXSR:
     *   CR4 bit 9
     *
     * GCC may generate SSE/SSE2 instructions for x86-64
     * code. CR4.OSFXSR must therefore be enabled before
     * executing compiled C code that uses those instructions.
     */
    /* CR4: PAE (bit 5), OSFXSR (bit 9), OSXMMEXCPT (bit 10) = 0x620 */
    movl %cr4, %eax
    orl $0x620, %eax
    movl %eax, %cr4

    movl $pml4_table, %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* CR0: limpiar EM (bit 2), fijar MP (bit 1), PG (bit 31), PE (bit 0) */
    movl %cr0, %eax
    andl $~(1 << 2), %eax
    orl $(1 << 1), %eax
    orl $0x80000001, %eax
    movl %eax, %cr0

    ret


.align 8

gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF

gdt64_end:

gdt64_desc:
    .word gdt64_end - gdt64 - 1
    .long gdt64


.section .text
.code64

long_mode_start:

    /* Cargar selectores de segmento de datos de 64 bits canonicos */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    xorq %rbp, %rbp

    andq $-16, %rsp

    call kernel_main

halt:
    hlt
    jmp halt

/* ====================================================================
 * Stubs de Interrupcion y Excepciones x86_64
 * ==================================================================== */
.macro ISR_NOERR index
.global isr_stub_\index
isr_stub_\index:
    pushq $0
    pushq $\index
    jmp isr_common_stub
.endm

.macro ISR_ERR index
.global isr_stub_\index
isr_stub_\index:
    pushq $\index
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33

.extern isr_exception_handler

isr_common_stub:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    movq %rsp, %rdi
    cld
    call isr_exception_handler

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    addq $16, %rsp
    iretq

.section .rodata
.align 8
.global isr_stub_table
isr_stub_table:
    .quad isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    .quad isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    .quad isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    .quad isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    .quad isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    .quad isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    .quad isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    .quad isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    .quad isr_stub_32, isr_stub_33

.section .text


.section .bss

.align 16

stack_bottom:
    .skip 65536

stack_top:

.align 4096

pml4_table:
    .skip 4096

pdpt_table:
    .skip 4096

pd_table:
    .skip 8192

.section .note.GNU-stack,"",@progbits
