#ifndef MYOS_TCP_H
#define MYOS_TCP_H

#include <stdint.h>

#define TCP_EOF   (-1)    /* el peer cerro y no quedan datos */
#define TCP_ERR   (-2)    /* conexion reseteada o invalida   */

/*
 * Conexion activa. Devuelve un manejador (>= 0) o -1 si falla.
 * timeout_ms se reparte en 3 intentos de SYN.
 */
int tcp_connect(const uint8_t *dst_ip, uint16_t dst_port, uint32_t timeout_ms);

/*
 * Envia len bytes (stop-and-wait: un segmento cada vez, con
 * retransmision). Devuelve len si todo fue confirmado, -1 si falla.
 */
int tcp_send(int h, const void *data, uint32_t len);

/*
 * > 0  bytes copiados
 *   0  timeout sin datos
 * TCP_EOF  el peer cerro y no quedan datos
 * TCP_ERR  conexion reseteada o manejador invalido
 */
int tcp_recv(int h, void *buf, uint16_t max, uint32_t timeout_ms);

/* Cierre ordenado (FIN) y liberacion del manejador. */
int tcp_close(int h);

/* Llamada desde ip.c con el segmento TCP (cabecera incluida). */
void tcp_input(
    const uint8_t *src_ip,
    const uint8_t *dst_ip,
    const uint8_t *tcp,
    uint16_t len
);

/* Prueba: conecta con el servidor LLM y envia un GET en bruto. */
void net_run_tcp_test(void);

#endif
