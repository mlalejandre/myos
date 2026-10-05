#ifndef MYOS_MUTEX_H
#define MYOS_MUTEX_H

#include <stdint.h>
#include "thread.h"

struct kmutex {
    volatile int  locked;
    struct tcb   *owner;
    const char   *name;
};

/* Inicializa un mutex con nombre descriptivo */
void kmutex_init(struct kmutex *m, const char *name);

/* Bloquea el mutex; si esta ocupado cede voluntariamente la CPU (thread_yield) */
void kmutex_lock(struct kmutex *m);

/* Intenta adquirir el mutex sin bloquear (devuelve 1 si OK, 0 si ocupado) */
int kmutex_trylock(struct kmutex *m);

/* Libera el mutex */
void kmutex_unlock(struct kmutex *m);

/* Auto-test de exclusion mutua concurrente con dos hilos */
int kmutex_test_self(void);

#endif
