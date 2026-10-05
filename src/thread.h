#ifndef MYOS_THREAD_H
#define MYOS_THREAD_H

#include <stdint.h>
#include <stddef.h>

#define THREAD_STATE_READY    0
#define THREAD_STATE_RUNNING  1
#define THREAD_STATE_SLEEPING 2
#define THREAD_STATE_DEAD     3

typedef void (*thread_func_t)(void *arg);

struct tcb {
    uint64_t        rsp;            /* Offset 0: Puntero de pila guardado */
    uint32_t        tid;            /* Identificador unico */
    uint32_t        state;          /* READY, RUNNING, SLEEPING, DEAD */
    char            name[32];       /* Nombre descriptivo */
    thread_func_t   entry;          /* Funcion de entrada */
    void           *arg;            /* Argumento */
    uint64_t        sleep_until_ms; /* Despertar en uptime ms */
    uint64_t        stack_slot;     /* Base virtual de la ventana de pila */
    uint64_t        ticks_run;      /* Estadistica de ejecucion */
    struct tcb     *next;           /* Siguiente en lista circular */
};

/* Inicializa el subsistema de hilos, Hilo 0 (kernel_main), Hilo 1 (idle) e Hilo 2 (netd) */
void thread_init(void);

/* Crea un nuevo hilo de kernel con pila propia y pagina de guarda */
struct tcb *thread_create(const char *name, thread_func_t entry, void *arg);

/* Cede voluntariamente el control al siguiente hilo disponible */
void thread_yield(void);

/* Pone el hilo actual en reposo durante N milisegundos */
void thread_sleep(uint32_t ms);

/* Finaliza el hilo actual marcandolo como DEAD */
void thread_exit(void);

/* Devuelve el TCB del hilo actualmente en ejecucion */
struct tcb *thread_current(void);

/* Recolecta y libera los recursos de los hilos finalizados (DEAD) */
void thread_reap_dead(void);

/* Imprime el estado de todos los hilos del sistema */
void thread_dump(void);

/* Auto-test con demonios de fondo y ciclo de vida de hilo efimero con recoleccion */
int thread_test_self(void);

#endif
