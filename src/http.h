#ifndef MYOS_HTTP_H
#define MYOS_HTTP_H

#include <stdint.h>

struct http_response {
    int      status_code;     /* 200, 404, etc. -1 si hay error o timeout */
    char    *body;            /* Puntero al inicio del body dentro del buffer */
    uint32_t body_len;        /* Longitud del body en bytes */
};

/*
 * Peticion HTTP generica (GET / POST).
 * buf: buffer de recepcion proporcionado por el llamante.
 * buf_size: tamano maximo del buffer.
 * resp: estructura rellenada con status_code, body y body_len.
 * Devuelve status_code (>= 0) o -1 en caso de error.
 */
int http_request(
    const uint8_t *ip,
    uint16_t port,
    const char *method,
    const char *path,
    const char *content_type,   /* opcional, ej: "application/json" */
    const char *body,           /* opcional para POST */
    uint32_t body_len,
    char *buf,
    uint32_t buf_size,
    struct http_response *resp,
    uint32_t timeout_ms
);

int http_get_host(
    const uint8_t *ip,
    uint16_t port,
    const char *host,
    const char *path,
    char *buf,
    uint32_t buf_size,
    struct http_response *resp,
    uint32_t timeout_ms
);

int http_get(
    const uint8_t *ip,
    uint16_t port,
    const char *path,
    char *buf,
    uint32_t buf_size,
    struct http_response *resp,
    uint32_t timeout_ms
);

int http_post_json(
    const uint8_t *ip,
    uint16_t port,
    const char *path,
    const char *json_body,
    char *buf,
    uint32_t buf_size,
    struct http_response *resp,
    uint32_t timeout_ms
);

#endif
