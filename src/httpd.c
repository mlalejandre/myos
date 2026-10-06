#include "httpd.h"
#include "tcp.h"
#include "console.h"
#include "fs.h"
#include "mem.h"
#include "pmm.h"
#include "sysinfo.h"
#include "thread.h"
#include "shell.h"
#include "rtc.h"
#include "idt.h"

static int httpd_listen_h = -1;
static uint32_t httpd_hits = 0;

static const char *httpd_strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (!*n) return haystack;
    }
    return 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static uint32_t my_strlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void buf_append(char *dst, uint32_t max, uint32_t *pos, const char *src)
{
    while (*src && *pos < max - 1) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
}

static void buf_append_dec(char *dst, uint32_t max, uint32_t *pos, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        if (*pos < max - 1) dst[(*pos)++] = '0';
        dst[*pos] = '\0';
        return;
    }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && *pos < max - 1) dst[(*pos)++] = tmp[--n];
    dst[*pos] = '\0';
}

static void serve_client(int h)
{
    static char rx_buf[2048];
    static char resp_body[4096];
    static char resp_hdr[512];

    int n = tcp_recv(h, rx_buf, sizeof(rx_buf) - 1, 1500);
    if (n <= 0) return;
    rx_buf[n] = '\0';
    httpd_hits++;

    /* Extraer ruta HTTP (GET /ruta ...) */
    char path[128];
    path[0] = '\0';
    if (rx_buf[0] == 'G' && rx_buf[1] == 'E' && rx_buf[2] == 'T' && rx_buf[3] == ' ') {
        const char *p = rx_buf + 4;
        while (*p == ' ') p++;
        uint32_t pp = 0;
        while (*p && *p != ' ' && *p != '?' && pp < sizeof(path) - 1) {
            path[pp++] = *p++;
        }
        path[pp] = '\0';
    }

    uint32_t body_pos = 0;
    const char *content_type = "text/html; charset=utf-8";
    int status_code = 200;

    if (str_eq(path, "/") || str_eq(path, "/index.html")) {
        buf_append(resp_body, sizeof(resp_body), &body_pos,
            "<!DOCTYPE html>\n<html>\n<head>\n"
            "<title>SOMA 0.2 :: Bare-Metal Web Dashboard</title>\n"
            "<meta charset='utf-8'>\n"
            "<style>\n"
            "body{background:#0d1117;color:#c9d1d9;font-family:monospace;padding:24px;margin:0;}\n"
            "h1{color:#58a6ff;border-bottom:1px solid #30363d;padding-bottom:12px;}\n"
            ".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:18px;margin-bottom:18px;}\n"
            ".badge{background:#238636;color:#fff;padding:4px 10px;border-radius:12px;font-size:13px;font-weight:bold;}\n"
            "pre{background:#090d13;padding:14px;border-radius:6px;color:#7ee787;overflow-x:auto;}\n"
            "a{color:#58a6ff;text-decoration:none;margin-right:16px;font-weight:bold;}\n"
            "a:hover{text-decoration:underline;}\n"
            "</style>\n</head>\n<body>\n"
            "<h1>SOMA 0.2 <span class='badge'>Ring 0 HTTP Server</span></h1>\n"
            "<div class='card'>\n"
            "<h3>Sistema Operativo Multi-Agente (x86_64 Long Mode)</h3>\n"
            "<p>Servidor HTTP bare-metal atendiendo en puerto 80 nativo desde memoria f&iacute;sica sin POSIX ni Linux.</p>\n"
            "<p>\n"
            "<a href='/proc/uptime'>/proc/uptime</a>\n"
            "<a href='/proc/meminfo'>/proc/meminfo</a>\n"
            "<a href='/proc/cpuinfo'>/proc/cpuinfo</a>\n"
            "<a href='/proc/threads'>/proc/threads</a>\n"
            "<a href='/proc/version'>/proc/version</a>\n"
            "<a href='/dev/rtc'>/dev/rtc</a>\n"
            "<a href='/api/status'>/api/status (JSON)</a>\n"
            "</p>\n</div>\n"
            "<div class='card'>\n"
            "<h3>Telemetr&iacute;a de Kernel (somafetch)</h3>\n<pre>\n"
        );
        show_somafetch(resp_body + body_pos, sizeof(resp_body) - body_pos - 64);
        body_pos = my_strlen(resp_body);
        buf_append(resp_body, sizeof(resp_body), &body_pos, "</pre>\n</div>\n</body>\n</html>\n");
    } else if (str_eq(path, "/api/status")) {
        content_type = "application/json";
        uint64_t ms = timer_get_uptime_ms();
        size_t f_free = 0, f_used = 0, f_total = 0;
        pmm_get_stats(&f_free, &f_used, &f_total);

        buf_append(resp_body, sizeof(resp_body), &body_pos, "{\n");
        buf_append(resp_body, sizeof(resp_body), &body_pos, "  \"os\": \"SOMA\",\n");
        buf_append(resp_body, sizeof(resp_body), &body_pos, "  \"version\": \"0.2-release\",\n");
        buf_append(resp_body, sizeof(resp_body), &body_pos, "  \"arch\": \"x86_64\",\n");
        buf_append(resp_body, sizeof(resp_body), &body_pos, "  \"uptime_ms\": ");
        buf_append_dec(resp_body, sizeof(resp_body), &body_pos, ms);
        buf_append(resp_body, sizeof(resp_body), &body_pos, ",\n  \"ram_used_mb\": ");
        buf_append_dec(resp_body, sizeof(resp_body), &body_pos, (f_used * 4096ULL) / (1024ULL * 1024ULL));
        buf_append(resp_body, sizeof(resp_body), &body_pos, ",\n  \"ram_total_mb\": ");
        buf_append_dec(resp_body, sizeof(resp_body), &body_pos, (f_total * 4096ULL) / (1024ULL * 1024ULL));
        buf_append(resp_body, sizeof(resp_body), &body_pos, ",\n  \"http_hits\": ");
        buf_append_dec(resp_body, sizeof(resp_body), &body_pos, (uint64_t)httpd_hits);
        buf_append(resp_body, sizeof(resp_body), &body_pos, "\n}\n");
    } else {
        /* Servir archivo de /proc, /dev o RamFS como texto plano */
        if (httpd_strstr(path, "/etc/") || httpd_strstr(path, "/agent/") || httpd_strstr(path, "/tmp/")) {
            status_code = 403;
            buf_append(resp_body, sizeof(resp_body), &body_pos, "403 Forbidden - Access Denied\n");
        } else {
            content_type = "text/plain; charset=utf-8";
            int r = vfs_read(path, resp_body, sizeof(resp_body) - 1);
            if (r >= 0) {
                body_pos = (uint32_t)r;
                resp_body[body_pos] = '\0';
            } else {
                status_code = 404;
                buf_append(resp_body, sizeof(resp_body), &body_pos, "404 Not Found - SOMA Bare-Metal VFS\n");
            }
        }
    }

    /* Construir cabeceras HTTP */
    uint32_t hdr_pos = 0;
    if (status_code == 200) {
        buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "HTTP/1.1 200 OK\r\n");
    } else {
        buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "HTTP/1.1 404 Not Found\r\n");
    }
    buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "Server: SOMA-Kernel/0.2\r\n");
    buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "Content-Type: ");
    buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, content_type);
    buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "\r\nContent-Length: ");
    buf_append_dec(resp_hdr, sizeof(resp_hdr), &hdr_pos, (uint64_t)body_pos);
    buf_append(resp_hdr, sizeof(resp_hdr), &hdr_pos, "\r\nConnection: close\r\n\r\n");

    tcp_send(h, resp_hdr, hdr_pos);
    if (body_pos > 0) {
        tcp_send(h, resp_body, body_pos);
    }
}

void httpd_thread_func(void *arg)
{
    (void)arg;
    httpd_listen_h = tcp_listen(80);
    if (httpd_listen_h < 0) {
        kprint("HTTPD: Error abriendo socket listen en puerto 80\n");
        return;
    }
    kprint("HTTPD: Demonio web activo en puerto 80 (TID 4)\n");

    for (;;) {
        int client = tcp_accept(httpd_listen_h, 200);
        if (client >= 0) {
            serve_client(client);
            tcp_close(client);
        }
        thread_sleep(10);
    }
}

void httpd_init(void)
{
    thread_create("httpd", httpd_thread_func, 0);
}

int cmd_httpd(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    uint32_t p = 0;
    kprint("\nESTADO DEL SERVIDOR WEB HTTPD:\n-------------------------------\n");
    kprint("  Puerto TCP:       80 (reenviado a localhost:8090 en el host)\n");
    kprint("  Estado:           ESCUCHANDO (Daemon KThread activo)\n");
    kprint("  Peticiones (Hits): "); kprint_dec(httpd_hits); kprint("\n");
    kprint("  Panel URL:        http://localhost:8090/\n");
    kprint("-------------------------------\n");

    buf_append(out_buf, max_out, &p, "HTTPD: Puerto 80 activo | Hits: ");
    buf_append_dec(out_buf, max_out, &p, (uint64_t)httpd_hits);
    return 1;
}
