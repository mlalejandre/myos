# ESPECIFICACIÓN ARQUITECTÓNICA DE PORTABILIDAD (x86_64 / ARM64)

MYOS adopta un diseño desacoplado donde el **80% del código del sistema operativo es 100% agnóstico a la arquitectura**:
+------------------------------------------------------------------------+
| CAPA AGNÓSTICA (80% DEL CÓDIGO) |
| |
| - Agente Autónomo ReAct & Loop de Modificación (kernel.c, llm.c) |
| - Pila de Red TCP/IP (tcp.c, udp.c, ip.c, dns.c, http.c, net.c) |
| - Sistema de Archivos RamFS Persistente & Dirty-Track (fs.c) |
| - Heap Allocator de Memoria Dinámica kmalloc/kfree (mem.c) |
| - Planificador Cooperativo Round-Robin & KThreads (thread.c) |
| - Primitivas de Sincronización y Recuperación KMutex (mutex.c) |
| - Suite de Coreutils, Editor Nano y Shell Readline |
+------------------------------------------------------------------------+
|
[ src/arch.h - Hardware Abstraction Layer ]
|
+------------------------------------+-----------------------------------+
| SUBÁRBOL x86_64 (20%) | SUBÁRBOL ARM64 (20%) |
| | |
| - Bootstrap Multiboot (boot.s) | - Arranque EL1 bare-metal |
| - Cambio de contexto (switch.s) | - Cambio contexto (x19-x30, sp) |
| - IDT, PIC 8259 & PIT (idt.c) | - Vector Table VBAR_EL1 & GICv2/3 |
| - Paginación PML4 / CR3 (vmm.c) | - Paginación TTBR0_EL1 4 KiB |
| - Puertos I/O 0x3F8/COM1 (io.h) | - MMIO UART PL011 0x09000000 |
| - Emulación QEMU 'pc' x86_64 | - Emulación QEMU 'virt' AArch64 |
+------------------------------------+-----------------------------------+

code
Code
## Contrato de la Capa de Abstracción de Hardware (`src/arch.h`)

| Primitiva | Implementación x86_64 | Implementación ARM64 (Cortex-A72) |
| :--- | :--- | :--- |
| `arch_pause()` | `pause` | `isb` / `yield` |
| `arch_halt()` | `hlt` | `wfi` (Wait For Interrupt) |
| `arch_interrupts_enable()` | `sti` | `msr daifclr, #2` |
| `arch_interrupts_disable()` | `cli` | `msr daifset, #2` |
| `arch_cycle_counter()` | `rdtsc` | `mrs %0, cntvct_el0` |
| `arch_memory_barrier()` | `mfence` | `dmb ish` |
