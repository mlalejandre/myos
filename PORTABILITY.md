# ESPECIFICACIÓN ARQUITECTÓNICA DE PORTABILIDAD SOMA (x86_64 / ARM64)

SOMA adopta un diseño desacoplado donde el 80% del código del sistema operativo es agnóstico a la arquitectura:

- Orquestación Jerárquica Multi-Agente & Singularity Loop (agent.c)
- Motor de Búsqueda Semántica BM25 & Tokenizador UTF-8 (fs.c)
- Pila de Red TCP/IP Nativa (tcp.c, udp.c, ip.c, dns.c, http.c, net.c)
- Servidor Web Dashboard HTML5/REST en Ring 0 (httpd.c)
- Sistema de Archivos RamFS V2 SOMAFS02 (64 inodos / 16 KiB) (fs.c)
- Heap Allocator KHeap desacoplado en 0x20000000 (mem.c)
- Planificador Cooperativo Round-Robin & KThreads (thread.c)
- Primitivas de Sincronización y Recuperación KMutex (mutex.c)
- Suite de Coreutils, Editor Nano y Shell Readline

## Contrato HAL (src/arch.h)

- arch_pause(): pause (x86_64) / yield (AArch64)
- arch_halt(): hlt (x86_64) / wfi (AArch64)
- arch_interrupts_enable(): sti (x86_64) / daifclr (AArch64)
- arch_interrupts_disable(): cli (x86_64) / daifset (AArch64)
- arch_cycle_counter(): rdtsc (x86_64) / cntvct_el0 (AArch64)
- arch_memory_barrier(): mfence (x86_64) / dmb ish (AArch64)
