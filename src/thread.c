#include "fs.h"
#include "sysinfo.h"
#include "thread.h"
#include "vmm.h"
#include "pmm.h"
#include "mem.h"
#include "console.h"
#include "idt.h"
#include "net.h"

/* Prototipos de switch.s */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline_asm(void);

#define THREAD_STACK_BASE  0x0000000080000000ULL
#define THREAD_SLOT_SIZE   0x00010000ULL /* 64 KiB */
#define THREAD_STACK_PAGES 8             /* 12 KiB reales de pila */

static struct tcb *thread_list = 0;
static struct tcb *curr_thread = 0;
static struct tcb *idle_tcb = 0;
static uint32_t    next_tid = 0;
static int         multitasking_active = 0;

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

struct tcb *thread_current(void)
{
    return curr_thread;
}

void thread_trampoline_c(void)
{
    if (curr_thread && curr_thread->entry) {
        curr_thread->entry(curr_thread->arg);
    }
}

static uint64_t allocate_thread_stack(uint32_t tid)
{
    uint64_t slot = THREAD_STACK_BASE + (uint64_t)tid * THREAD_SLOT_SIZE;

    /* 1. Desmapear la pagina de guarda (offset 0) */
    vmm_unmap_page(slot);

    /* 2. Mapear 3 paginas consecutivas (12 KiB) a partir de slot + 4096 */
    for (uint64_t p = 1; p <= THREAD_STACK_PAGES; ++p) {
        uintptr_t frame = pmm_alloc_frame();
        if (!frame) return 0;

        uint64_t vaddr = slot + p * VMM_PAGE_SIZE;
        if (vmm_map_page(vaddr, (uint64_t)frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NX) != 0) {
            pmm_free_frame(frame);
            return 0;
        }
    }

    return slot + (uint64_t)(THREAD_STACK_PAGES + 1) * VMM_PAGE_SIZE;
}

static void free_thread_stack(uint64_t slot)
{
    if (slot < THREAD_STACK_BASE) return;

    for (uint64_t p = 1; p <= THREAD_STACK_PAGES; ++p) {
        uint64_t vaddr = slot + p * VMM_PAGE_SIZE;
        uint64_t phys  = vmm_virt_to_phys(vaddr);
        vmm_unmap_page(vaddr);
        if (phys) {
            pmm_free_frame((uintptr_t)phys);
        }
    }
}

struct tcb *thread_create(const char *name, thread_func_t entry, void *arg)
{
    struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!t) return 0;

    memset(t, 0, sizeof(struct tcb));
    t->tid = next_tid++;
    str_copy(t->name, name ? name : "kthread", sizeof(t->name));
    t->entry = entry;
    t->arg = arg;
    t->state = THREAD_STATE_READY;
    t->stack_slot = THREAD_STACK_BASE + (uint64_t)t->tid * THREAD_SLOT_SIZE;
    t->ticks_run = 0;

    uint64_t stack_top = allocate_thread_stack(t->tid);
    if (!stack_top) {
        kfree(t);
        return 0;
    }

    uint64_t *sp = (uint64_t *)stack_top;
    /* Orden inverso a switch_context: rip, rbp, rbx, r12-r15, rflags (el ultimo se pop primero) */
    *(--sp) = (uint64_t)thread_trampoline_asm;
    *(--sp) = 0;    /* rbp */
    *(--sp) = 0;    /* rbx */
    *(--sp) = 0;    /* r12 */
    *(--sp) = 0;    /* r13 */
    *(--sp) = 0;    /* r14 */
    *(--sp) = 0;    /* r15 */
    *(--sp) = 0x202ULL;   /* rflags (IF=1) */

    t->rsp = (uint64_t)sp;

    if (!thread_list) {
        thread_list = t;
        t->next = t;
    } else {
        struct tcb *last = thread_list;
        while (last->next != thread_list) {
            last = last->next;
        }
        last->next = t;
        t->next = thread_list;
    }

    return t;
}

void thread_reap_dead(void)
{
    if (!thread_list) return;

    struct tcb *prev = thread_list;
    struct tcb *curr = thread_list->next;

    /* No recolectar hilo 0 (kernel_main) ni hilo 1 (idle) ni el hilo actualmente en ejecucion */
    while (curr != thread_list) {
        if (curr != curr_thread && curr->state == THREAD_STATE_DEAD) {
            struct tcb *to_free = curr;
            prev->next = curr->next;
            curr = curr->next;

            free_thread_stack(to_free->stack_slot);
            kfree(to_free);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

/* Hilo 1: Idle Task en reposo de CPU con HLT */
static void idle_thread_func(void *arg)
{
    (void)arg;
    for (;;) {
        thread_reap_dead();
        __asm__ volatile ("sti; hlt");
        thread_yield();
    }
}

/* Hilo 2: VirtIO-NET Daemon de recepcion en segundo plano */
static void netd_thread_func(void *arg)
{
    (void)arg;
    for (;;) {
        while (net_poll()) {
            /* Vaciar cola de recepcion */
        }
        thread_sleep(10);
    }
}


/* Hilo 3: Demonio de Telemetria y Monitorizacion en Segundo Plano */
static void sysmon_thread_func(void *arg)
{
    (void)arg;
    static char tele_buf[256];

    for (;;) {
        thread_sleep(2000); /* Ejecutar cada 2 segundos */

        size_t f_free = 0, f_used = 0, f_total = 0;
        pmm_get_stats(&f_free, &f_used, &f_total);

        size_t h_used = 0, h_free = 0;
        kheap_stats(&h_used, &h_free);

        uint64_t uptime_s = timer_get_uptime_ms() / 1000ULL;

        /* Formatear reporte de telemetria en vivo */
        uint32_t p = 0;
        const char *hdr = "[MYOS TELEMETRY BACKGROUND LOG]\n- Uptime: ";
        while (*hdr) tele_buf[p++] = *hdr++;

        /* Segundos de uptime */
        uint64_t v = uptime_s;
        if (v == 0) tele_buf[p++] = '0';
        else { char b[12]; int n = 0; while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; } while (n) tele_buf[p++] = b[--n]; }

        const char *m2 = "s\n- RAM Libre: ";
        while (*m2) tele_buf[p++] = *m2++;
        v = (f_free * 4096ULL) / (1024ULL * 1024ULL);
        if (v == 0) tele_buf[p++] = '0';
        else { char b[12]; int n = 0; while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; } while (n) tele_buf[p++] = b[--n]; }

        const char *m3 = " MiB\n- Heap Libre: ";
        while (*m3) tele_buf[p++] = *m3++;
        v = h_free / 1024ULL;
        if (v == 0) tele_buf[p++] = '0';
        else { char b[12]; int n = 0; while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; } while (n) tele_buf[p++] = b[--n]; }

        const char *m4 = " KiB\n- Estado: 100% OPERATIVO (sysmon bg task)\n";
        while (*m4) tele_buf[p++] = *m4++;
        tele_buf[p] = '\0';

        /* Escribir en VFS de forma concurrente protegida */
        vfs_write("/sys/telemetry.txt", tele_buf, p);
    }
}

void thread_init(void)
{
    if (multitasking_active) return;

    /* Hilo 0: kernel_main */
    struct tcb *kmain = (struct tcb *)kmalloc(sizeof(struct tcb));
    memset(kmain, 0, sizeof(struct tcb));
    kmain->tid = next_tid++;
    str_copy(kmain->name, "kernel_main", sizeof(kmain->name));
    kmain->state = THREAD_STATE_RUNNING;
    kmain->next = kmain;

    uint64_t cur_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(cur_rsp));
    kmain->rsp = cur_rsp;

    thread_list = kmain;
    curr_thread = kmain;
    multitasking_active = 1;

    /* Hilo 1: idle task */
    idle_tcb = thread_create("idle", idle_thread_func, 0);

    /* Hilo 2: net daemon */
    thread_create("netd", netd_thread_func, 0);

    /* Hilo 3: sysmon daemon */
    thread_create("sysmon", sysmon_thread_func, 0);

    kprint("KTHREADS: Inicializado (Hilo 0: 'kernel_main', Hilo 1: 'idle', Hilo 2: 'netd', Hilo 3: 'sysmon')\n");
}

static void schedule(void)
{
    if (!multitasking_active || !curr_thread) return;

    uint64_t now_ms = timer_get_uptime_ms();

    /* 1. Despertar hilos dormidos */
    struct tcb *scan = thread_list;
    if (scan) {
        do {
            if (scan->state == THREAD_STATE_SLEEPING && now_ms >= scan->sleep_until_ms) {
                scan->state = THREAD_STATE_READY;
            }
            scan = scan->next;
        } while (scan != thread_list);
    }

    /* 2. Buscar siguiente hilo READY */
    struct tcb *next = curr_thread->next;
    struct tcb *candidate = 0;

    while (next != curr_thread) {
        if (next->state == THREAD_STATE_READY && next != idle_tcb) {
            candidate = next;
            break;
        }
        next = next->next;
    }

    /* Si no hay hilos normales listos, comprobar si el actual puede continuar */
    if (!candidate) {
        if (curr_thread->state == THREAD_STATE_RUNNING && curr_thread != idle_tcb) {
            return;
        }
        /* Si nadie esta listo, ejecutar idle_tcb */
        if (idle_tcb && (idle_tcb->state == THREAD_STATE_READY || idle_tcb->state == THREAD_STATE_RUNNING)) {
            candidate = idle_tcb;
        } else {
            return;
        }
    }

    if (candidate == curr_thread) return;

    /* 3. Conmutar contexto */
    struct tcb *prev = curr_thread;
    if (prev->state == THREAD_STATE_RUNNING) {
        prev->state = THREAD_STATE_READY;
    }
    candidate->state = THREAD_STATE_RUNNING;
    candidate->ticks_run++;
    curr_thread = candidate;

    switch_context(&prev->rsp, candidate->rsp);
}

void thread_yield(void)
{
    schedule();
}

void thread_sleep(uint32_t ms)
{
    if (!curr_thread) return;
    curr_thread->sleep_until_ms = timer_get_uptime_ms() + (uint64_t)ms;
    curr_thread->state = THREAD_STATE_SLEEPING;
    schedule();
}

void thread_exit(void)
{
    if (!curr_thread) return;
    curr_thread->state = THREAD_STATE_DEAD;
    schedule();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void thread_dump(void)
{
    kprint("\nTABLA DE HILOS (KTHREADS):\n");
    kprint("------------------------------------------------------------------\n");
    kprint(" TID  Nombre           Estado     Ticks CPU  RSP Virtual  Pila Slot\n");
    kprint("------------------------------------------------------------------\n");

    if (!thread_list) {
        kprint("  (sin hilos)\n");
        return;
    }

    struct tcb *t = thread_list;
    do {
        kprint("  ");
        kprint_dec(t->tid);
        if (t->tid < 10) kprint("  "); else kprint(" ");

        kprint(t->name);
        uint32_t nl = 0; while (t->name[nl]) nl++;
        for (uint32_t s = nl; s < 17; ++s) kputc(' ');

        if (t->state == THREAD_STATE_RUNNING)  kprint("RUNNING    ");
        else if (t->state == THREAD_STATE_READY)    kprint("READY      ");
        else if (t->state == THREAD_STATE_SLEEPING) kprint("SLEEPING   ");
        else if (t->state == THREAD_STATE_DEAD)     kprint("DEAD       ");
        else                                        kprint("UNKNOWN    ");

        kprint_dec((uint32_t)t->ticks_run);
        uint32_t tl = (t->ticks_run < 10) ? 1 : ((t->ticks_run < 100) ? 2 : ((t->ticks_run < 1000) ? 3 : 5));
        for (uint32_t s = tl; s < 11; ++s) kputc(' ');
        kprint("0x"); kprint_hex32((uint32_t)t->rsp);
        kprint("   0x"); kprint_hex32((uint32_t)t->stack_slot);
        kprint("\n");

        t = t->next;
    } while (t != thread_list);

    kprint("------------------------------------------------------------------\n");
    kprint("Hilo activo: TID ");
    kprint_dec(curr_thread ? curr_thread->tid : 0);
    kprint(" ('"); kprint(curr_thread ? curr_thread->name : "none"); kprint("')\n\n");
}

/* Auto-test: hilo efimero con recoleccion de basura */
static volatile int ephemeral_executed = 0;

static void ephemeral_func(void *arg)
{
    (void)arg;
    ephemeral_executed = 1;
    thread_sleep(10);
    /* Sale implicitamente por thread_trampoline -> thread_exit() */
}

int thread_test_self(void)
{
    kprint("\n[KTHREAD REAPER TEST] Verificando ciclo de vida de hilo efimero...\n");

    size_t f_before = 0, u_before = 0, t_before = 0;
    pmm_get_stats(&f_before, &u_before, &t_before);

    ephemeral_executed = 0;
    struct tcb *ephemeral = thread_create("transient_task", ephemeral_func, 0);
    if (!ephemeral) {
        kprint("  FALLO: No se pudo crear hilo efimero\n");
        return 0;
    }

    uint64_t ep_slot = ephemeral->stack_slot;

    /* Esperar a que ejecute y termine */
    uint64_t start = timer_get_uptime_ms();
    while (!ephemeral_executed && (timer_get_uptime_ms() - start < 1000)) {
        thread_yield();
    }

    if (!ephemeral_executed) {
        kprint("  FALLO: Hilo efimero no ejecuto\n");
        return 0;
    }

    /* Dar tiempo para que pase a DEAD */
    thread_sleep(20);

    /* Forzar recoleccion de basura */
    thread_reap_dead();

    /* Verificar que la pila fue desmapeada */
    if (vmm_virt_to_phys(ep_slot + 4096) != 0) {
        kprint("  FALLO: La pila del hilo muerto no fue desmapeada\n");
        return 0;
    }

    size_t f_after = 0, u_after = 0, t_after = 0;
    pmm_get_stats(&f_after, &u_after, &t_after);

    if (f_after != f_before) {
        kprint("  FALLO: Fuga de memoria física detectada en el ciclo de vida del hilo\n");
        return 0;
    }

    kprint("  Hilo efimero ejecutado, terminado y recolectado OK (0 fugas PMM)\n");
    kprint("[KTHREAD REAPER TEST] SUPERADO CON EXITO (Gestion de Recursos OK).\n\n");
    return 1;
}
