#ifndef MYOS_BOOT_GATE_H
#define MYOS_BOOT_GATE_H

#include <stdint.h>

/* Ejecuta la suite de pruebas bare-metal (0 = OK, -1 = fallo critico) */
int boot_gate_run_test_suite(char *out_buf, uint32_t max_out);

/* Verificacion del modo canary en el arranque */
void boot_gate_check(void);

#endif
