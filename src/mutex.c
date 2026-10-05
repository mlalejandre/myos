#include "mutex.h"
#include "console.h"

void kmutex_init(struct kmutex *m, const char *name)
{
    if (!m) return;
    m->locked = 0;
    m->owner = 0;
    m->name = name ? name : "mutex";
}

void kmutex_lock(struct kmutex *m)
{
    if (!m) return;
    struct tcb *curr = thread_current();

    while (1) {
        if (!m->locked) {
            m->locked = 1;
            m->owner = curr;
            return;
        }
        if (m->owner == curr) {
            return; /* Adquisicion recursiva por el mismo hilo */
        }
        thread_yield();
    }
}

int kmutex_trylock(struct kmutex *m)
{
    if (!m) return 0;
    struct tcb *curr = thread_current();

    if (!m->locked) {
        m->locked = 1;
        m->owner = curr;
        return 1;
    }
    return 0;
}

void kmutex_unlock(struct kmutex *m)
{
    if (!m) return;
    m->locked = 0;
    m->owner = 0;
}

/* Auto-test de sincronizacion */
static struct kmutex test_mtx;
static volatile int shared_counter = 0;
static volatile int mtx_worker1_done = 0;
static volatile int mtx_worker2_done = 0;

static void mtx_worker_func(void *arg)
{
    int id = (int)(uintptr_t)arg;
    for (int i = 0; i < 50; ++i) {
        kmutex_lock(&test_mtx);
        int val = shared_counter;
        thread_yield(); /* Forzar conmutacion en seccion critica */
        shared_counter = val + 1;
        kmutex_unlock(&test_mtx);
    }
    if (id == 1) mtx_worker1_done = 1;
    if (id == 2) mtx_worker2_done = 1;
}

int kmutex_test_self(void)
{
    kprint("\n[KMUTEX AUTO-TEST] Verificando exclusion mutua con dos hilos en seccion critica...\n");

    kmutex_init(&test_mtx, "test_counter_lock");
    shared_counter = 0;
    mtx_worker1_done = 0;
    mtx_worker2_done = 0;

    thread_create("mtx_w1", mtx_worker_func, (void *)1);
    thread_create("mtx_w2", mtx_worker_func, (void *)2);

    while (!mtx_worker1_done || !mtx_worker2_done) {
        thread_yield();
    }

    /* Limpiar los dos hilos muertos con el reaper */
    thread_reap_dead();

    if (shared_counter != 100) {
        kprint("  FALLO: Condicion de carrera detectada (contador=");
        kprint_dec(shared_counter);
        kprint(", esperado 100)\n");
        return 0;
    }

    kprint("  Seccion critica protegida OK (100 incrementos concurrentes sin conflicto)\n");
    kprint("[KMUTEX AUTO-TEST] SUPERADO CON EXITO.\n\n");
    return 1;
}
