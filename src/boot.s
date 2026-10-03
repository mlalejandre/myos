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
    movl %cr4, %eax
    orl $0x220, %eax
    movl %eax, %cr4

    movl $pml4_table, %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    movl %cr0, %eax
    orl $0x80000000, %eax
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

    movq $stack_top, %rsp
    xorq %rbp, %rbp

    andq $-16, %rsp

    call kernel_main

halt:
    hlt
    jmp halt


.section .bss

.align 16

stack_bottom:
    .skip 16384

stack_top:

.align 4096

pml4_table:
    .skip 4096

pdpt_table:
    .skip 4096

pd_table:
    .skip 8192

.section .note.GNU-stack,"",@progbits
