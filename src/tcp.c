#include <stdint.h>

#include "console.h"
#include "io.h"
#include "ip.h"
#include "mem.h"
#include "net.h"
#include "tcp.h"


#define IP_PROTO_TCP     6

#define TCP_FIN          0x01
#define TCP_SYN          0x02
#define TCP_RST          0x04
#define TCP_PSH          0x08
#define TCP_ACK          0x10

#define TCP_HDR_MIN      20
#define TCP_HDR_MAX      24
#define TCP_MSS_MAX      1460
#define TCP_MSS_DEFAULT  536
#define TCP_RX_BUFFER    32768
#define TCP_MAX_CONN     2

#define TCP_RTO_MS       1000
#define TCP_MAX_RETRIES  5

/* Aproximacion bajo QEMU/TCG: ~1 tick de TSC por ns. */
extern uint64_t tsc_ticks_per_ms;
#define TSC_PER_MS       tsc_ticks_per_ms


enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK
};


struct tcp_conn {
    uint8_t           used;
    volatile uint8_t  state;
    volatile uint8_t  peer_fin;
    volatile uint8_t  reset;
    uint8_t           fin_sent;

    uint8_t           remote_ip[4];
    uint16_t          remote_port;
    uint16_t          local_port;
    uint16_t          mss;

    volatile uint32_t snd_una;
    uint32_t          snd_nxt;
    uint32_t          rcv_nxt;

    uint16_t          rx_head;
    volatile uint16_t rx_count;
    uint8_t           rx[TCP_RX_BUFFER];
};

static struct tcp_conn conns[TCP_MAX_CONN];

static uint16_t next_port = 49152;


static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}


static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}


static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}


static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}


static int seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}


static int seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}


static uint32_t csum_add(uint32_t sum, const uint8_t *p, uint16_t len)
{
    while (len > 1) {
        sum += (uint32_t)(((uint32_t)p[0] << 8) | p[1]);
        p += 2;
        len = (uint16_t)(len - 2);
    }

    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }

    return sum;
}


/* Checksum TCP con pseudo-cabecera; 0 = valido al verificar. */
static uint16_t tcp_checksum(
    const uint8_t *src_ip,
    const uint8_t *dst_ip,
    const uint8_t *seg,
    uint16_t len
)
{
    uint8_t ph[12];

    memcpy(ph, src_ip, 4);
    memcpy(ph + 4, dst_ip, 4);
    ph[8] = 0;
    ph[9] = IP_PROTO_TCP;
    wr16(ph + 10, len);

    uint32_t sum = csum_add(0, ph, sizeof(ph));
    sum = csum_add(sum, seg, len);

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}


/* ---------------------------------------------------------------- */
/* Emision                                                          */
/* ---------------------------------------------------------------- */

static void tcp_emit_raw(
    const uint8_t *dst_ip,
    uint16_t sport,
    uint16_t dport,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    uint16_t window,
    int mss_option,
    const uint8_t *payload,
    uint16_t plen
)
{
    if (plen > TCP_MSS_MAX) {
        return;
    }

    uint8_t seg[TCP_HDR_MAX + TCP_MSS_MAX];

    uint16_t hlen = mss_option ? TCP_HDR_MAX : TCP_HDR_MIN;

    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    wr16(seg + 14, window);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);

    if (mss_option) {
        seg[20] = 2;
        seg[21] = 4;
        wr16(seg + 22, TCP_MSS_MAX);
    }

    if (plen > 0) {
        memcpy(seg + hlen, payload, plen);
    }

    uint16_t total = (uint16_t)(hlen + plen);

    wr16(seg + 16, tcp_checksum(net_ip, dst_ip, seg, total));

    ip_send(dst_ip, IP_PROTO_TCP, seg, total);
}


static uint16_t rx_window(const struct tcp_conn *c)
{
    return (uint16_t)(TCP_RX_BUFFER - c->rx_count);
}


static void conn_emit(
    struct tcp_conn *c,
    uint8_t flags,
    uint32_t seq,
    int mss_option,
    const uint8_t *payload,
    uint16_t plen
)
{
    tcp_emit_raw(
        c->remote_ip,
        c->local_port,
        c->remote_port,
        seq,
        (flags & TCP_ACK) ? c->rcv_nxt : 0,
        flags,
        rx_window(c),
        mss_option,
        payload,
        plen
    );
}


/* RST para segmentos que no pertenecen a ninguna conexion. */
static void send_reset_unknown(
    const uint8_t *remote_ip,
    uint16_t remote_port,
    uint16_t local_port,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    uint16_t plen
)
{
    if (flags & TCP_RST) {
        return;
    }

    if (flags & TCP_ACK) {

        tcp_emit_raw(remote_ip, local_port, remote_port,
                     ack, 0, TCP_RST, 0, 0, 0, 0);

    } else {

        uint32_t a = seq + plen +
                     ((flags & TCP_SYN) ? 1U : 0U) +
                     ((flags & TCP_FIN) ? 1U : 0U);

        tcp_emit_raw(remote_ip, local_port, remote_port,
                     0, a, TCP_RST | TCP_ACK, 0, 0, 0, 0);
    }
}


/* ---------------------------------------------------------------- */
/* Conexiones y buffer de recepcion                                 */
/* ---------------------------------------------------------------- */

static struct tcp_conn *get_conn(int h)
{
    if (h < 0 || h >= TCP_MAX_CONN || !conns[h].used) {
        return 0;
    }

    return &conns[h];
}


static struct tcp_conn *find_conn(
    const uint8_t *remote_ip,
    uint16_t remote_port,
    uint16_t local_port
)
{
    for (int i = 0; i < TCP_MAX_CONN; ++i) {

        struct tcp_conn *c = &conns[i];

        if (c->used &&
            c->local_port == local_port &&
            c->remote_port == remote_port &&
            memcmp(c->remote_ip, remote_ip, 4) == 0)
        {
            return c;
        }
    }

    return 0;
}


static uint16_t rx_store(struct tcp_conn *c, const uint8_t *p, uint16_t n)
{
    uint16_t space = rx_window(c);

    if (n > space) {
        n = space;
    }

    for (uint16_t i = 0; i < n; ++i) {
        c->rx[(uint16_t)((c->rx_head + c->rx_count + i) % TCP_RX_BUFFER)] = p[i];
    }

    c->rx_count = (uint16_t)(c->rx_count + n);

    return n;
}


static int conn_alive(const struct tcp_conn *c)
{
    return c->state != TCP_CLOSED && c->state != TCP_SYN_SENT;
}


static uint16_t parse_peer_mss(const uint8_t *tcp, uint16_t hlen)
{
    uint16_t i = TCP_HDR_MIN;

    while (i < hlen) {

        uint8_t kind = tcp[i];

        if (kind == 0) {
            break;
        }

        if (kind == 1) {
            ++i;
            continue;
        }

        if (i + 1 >= hlen) {
            break;
        }

        uint8_t l = tcp[i + 1];

        if (l < 2) {
            break;
        }

        if (kind == 2 && l == 4 && i + 4 <= hlen) {
            return rd16(tcp + i + 2);
        }

        i = (uint16_t)(i + l);
    }

    return 0;
}


/* ---------------------------------------------------------------- */
/* Recepcion                                                        */
/* ---------------------------------------------------------------- */

void tcp_input(
    const uint8_t *src_ip,
    const uint8_t *dst_ip,
    const uint8_t *tcp,
    uint16_t len
)
{
    if (len < TCP_HDR_MIN) {
        return;
    }

    uint16_t hlen = (uint16_t)((tcp[12] >> 4) * 4);

    if (hlen < TCP_HDR_MIN || hlen > len) {
        return;
    }

    if (tcp_checksum(src_ip, dst_ip, tcp, len) != 0) {
        return;
    }

    uint16_t sport = rd16(tcp);
    uint16_t dport = rd16(tcp + 2);
    uint32_t seq   = rd32(tcp + 4);
    uint32_t ack   = rd32(tcp + 8);
    uint8_t  flags = tcp[13];

    const uint8_t *payload = tcp + hlen;
    uint16_t plen = (uint16_t)(len - hlen);

    struct tcp_conn *c = find_conn(src_ip, sport, dport);

    if (!c) {
        send_reset_unknown(src_ip, sport, dport, seq, ack, flags, plen);
        return;
    }

    if (c->state == TCP_CLOSED) {
        return;
    }

    /* ---- SYN_SENT: esperamos SYN+ACK ---- */
    if (c->state == TCP_SYN_SENT) {

        if (flags & TCP_ACK) {

            if (ack != c->snd_nxt) {

                if (!(flags & TCP_RST)) {
                    tcp_emit_raw(src_ip, dport, sport,
                                 ack, 0, TCP_RST, 0, 0, 0, 0);
                }

                return;
            }
        }

        if (flags & TCP_RST) {

            if (flags & TCP_ACK) {
                c->reset = 1;
                c->state = TCP_CLOSED;
            }

            return;
        }

        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {

            uint16_t peer_mss = parse_peer_mss(tcp, hlen);

            if (peer_mss == 0) {
                peer_mss = TCP_MSS_DEFAULT;
            }

            c->mss = peer_mss < TCP_MSS_MAX ? peer_mss : TCP_MSS_MAX;
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state = TCP_ESTABLISHED;

            conn_emit(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
        }

        return;
    }

    /* ---- Estados sincronizados ---- */

    if (flags & TCP_RST) {

        if (seq_le(c->rcv_nxt, seq) &&
            seq_lt(seq, c->rcv_nxt + TCP_RX_BUFFER))
        {
            c->reset = 1;
            c->state = TCP_CLOSED;
        }

        return;
    }

    if (flags & TCP_SYN) {

        /* SYN+ACK retransmitido: volver a confirmar. */
        conn_emit(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
        return;
    }

    if (!(flags & TCP_ACK)) {
        return;
    }

    /* Procesar ACK. */
    if (seq_lt(c->snd_una, ack) && !seq_lt(c->snd_nxt, ack)) {
        c->snd_una = ack;
    }

    if (c->fin_sent && c->snd_una == c->snd_nxt) {

        if (c->state == TCP_FIN_WAIT_1) {
            c->state = TCP_FIN_WAIT_2;
        } else if (c->state == TCP_CLOSING || c->state == TCP_LAST_ACK) {
            c->state = TCP_CLOSED;
            return;
        }
    }

    /* Datos y FIN. */
    if (plen == 0 && !(flags & TCP_FIN)) {
        return;
    }

    int can_rx = (c->state == TCP_ESTABLISHED ||
                  c->state == TCP_FIN_WAIT_1 ||
                  c->state == TCP_FIN_WAIT_2);

    if (seq == c->rcv_nxt && can_rx) {

        uint16_t n = rx_store(c, payload, plen);

        c->rcv_nxt += n;

        if (n == plen && (flags & TCP_FIN)) {

            c->rcv_nxt += 1;
            c->peer_fin = 1;

            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_CLOSING;
            } else if (c->state == TCP_FIN_WAIT_2) {
                /* Se omite TIME_WAIT: pasamos directamente a CLOSED. */
                c->state = TCP_CLOSED;
            }
        }
    }

    /* ACK inmediato; si seq no coincide actua como ACK duplicado. */
    conn_emit(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
}


/* ---------------------------------------------------------------- */
/* API                                                              */
/* ---------------------------------------------------------------- */

int tcp_connect(const uint8_t *dst_ip, uint16_t dst_port, uint32_t timeout_ms)
{
    int h = -1;

    for (int i = 0; i < TCP_MAX_CONN; ++i) {

        if (!conns[i].used) {
            h = i;
            break;
        }
    }

    if (h < 0) {
        return -1;
    }

    struct tcp_conn *c = &conns[h];

    memset(c, 0, sizeof(*c));

    uint32_t iss = (uint32_t)rdtsc();

    c->used = 1;
    memcpy(c->remote_ip, dst_ip, 4);
    c->remote_port = dst_port;
    c->local_port = next_port++;

    if (next_port >= 65000) {
        next_port = 49152;
    }

    c->mss = TCP_MSS_DEFAULT;
    c->snd_una = iss;
    c->snd_nxt = iss + 1;
    c->state = TCP_SYN_SENT;

    uint32_t per_attempt = timeout_ms / 3;

    if (per_attempt == 0) {
        per_attempt = 1;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {

        conn_emit(c, TCP_SYN, iss, 1, 0, 0);

        uint64_t start = rdtsc();
        uint64_t limit = (uint64_t)per_attempt * TSC_PER_MS;

        while ((rdtsc() - start) < limit) {

            if (c->state == TCP_ESTABLISHED) {
                kprint("TCP: established, local port ");
                kprint_dec(c->local_port);
                kprint(", MSS ");
                kprint_dec(c->mss);
                kprint("\n");
                return h;
            }

            if (c->reset || c->state == TCP_CLOSED) {
                kprint("TCP: connection refused (RST)\n");
                c->used = 0;
                return -1;
            }

            net_wait_step();
            cpu_pause();
        }
    }

    kprint("TCP: connect timeout\n");

    c->used = 0;

    return -1;
}


/* 1 = confirmado, 0 = timeout, -1 = conexion caida. */
static int tcp_wait_ack(struct tcp_conn *c, uint32_t target, uint32_t timeout_ms)
{
    uint64_t start = rdtsc();
    uint64_t limit = (uint64_t)timeout_ms * TSC_PER_MS;

    for (;;) {

        if (c->reset || c->state == TCP_CLOSED) {
            return -1;
        }

        if (!seq_lt(c->snd_una, target)) {
            return 1;
        }

        if ((rdtsc() - start) >= limit) {
            return 0;
        }

        net_wait_step();
        cpu_pause();
    }
}


int tcp_send(int h, const void *data, uint32_t len)
{
    struct tcp_conn *c = get_conn(h);

    if (!c) {
        return -1;
    }

    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;

    while (off < len) {

        uint32_t chunk = len - off;

        if (chunk > c->mss) {
            chunk = c->mss;
        }

        uint32_t seq = c->snd_nxt;
        uint32_t end = seq + chunk;

        c->snd_nxt = end;

        int acked = 0;

        for (int attempt = 0; attempt < TCP_MAX_RETRIES; ++attempt) {

            conn_emit(c, TCP_PSH | TCP_ACK, seq, 0, p + off, (uint16_t)chunk);

            int r = tcp_wait_ack(c, end, TCP_RTO_MS);

            if (r < 0) {
                kprint("TCP: connection lost while sending\n");
                return -1;
            }

            if (r == 1) {
                acked = 1;
                break;
            }
        }

        if (!acked) {
            kprint("TCP: send failed (no ACK)\n");
            return -1;
        }

        off += chunk;
    }

    return (int)len;
}


int tcp_recv(int h, void *buf, uint16_t max, uint32_t timeout_ms)
{
    struct tcp_conn *c = get_conn(h);

    if (!c) {
        return TCP_ERR;
    }

    if (max == 0) {
        return 0;
    }

    uint8_t *out = (uint8_t *)buf;

    uint64_t start = rdtsc();
    uint64_t limit = (uint64_t)timeout_ms * TSC_PER_MS;

    for (;;) {

        if (c->rx_count > 0) {

            uint16_t free_before = rx_window(c);
            uint16_t n = c->rx_count < max ? c->rx_count : max;

            for (uint16_t i = 0; i < n; ++i) {
                out[i] = c->rx[(uint16_t)((c->rx_head + i) % TCP_RX_BUFFER)];
            }

            c->rx_head = (uint16_t)((c->rx_head + n) % TCP_RX_BUFFER);
            c->rx_count = (uint16_t)(c->rx_count - n);

            /* Si la ventana estaba casi cerrada, avisar de que se abrio. */
            if (free_before < TCP_MSS_MAX && conn_alive(c)) {
                conn_emit(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
            }

            return n;
        }

        if (c->reset) {
            return TCP_ERR;
        }

        if (c->peer_fin || c->state == TCP_CLOSED) {
            return TCP_EOF;
        }

        if ((rdtsc() - start) >= limit) {
            return 0;
        }

        net_wait_step();
        cpu_pause();
    }
}


int tcp_close(int h)
{
    struct tcp_conn *c = get_conn(h);

    if (!c) {
        return -1;
    }

    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {

        uint32_t fin_seq = c->snd_nxt;

        conn_emit(c, TCP_FIN | TCP_ACK, fin_seq, 0, 0, 0);

        c->snd_nxt = fin_seq + 1;
        c->fin_sent = 1;
        c->state = (c->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1
                                                 : TCP_LAST_ACK;

        for (int round = 0; round < 3; ++round) {

            uint64_t start = rdtsc();
            uint64_t limit = (uint64_t)TCP_RTO_MS * TSC_PER_MS;

            while ((rdtsc() - start) < limit) {

                if (c->state == TCP_CLOSED) {
                    break;
                }

                net_wait_step();
                cpu_pause();
            }

            if (c->state == TCP_CLOSED) {
                break;
            }

            conn_emit(c, TCP_FIN | TCP_ACK, fin_seq, 0, 0, 0);
        }

        if (c->state != TCP_CLOSED) {
            /* El peer no cerro: abortar para no dejarlo colgado. */
            conn_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, 0, 0, 0);
        }
    }

    c->state = TCP_CLOSED;
    c->used = 0;

    return 0;
}


/* ---------------------------------------------------------------- */
/* Prueba                                                           */
/* ---------------------------------------------------------------- */

void net_run_tcp_test(void)
{
    kprint("\nNET: TCP TEST\n");
    kprint("-------------\n");

    if (!net_init()) {
        kprint("NET: init FAILED\n");
        return;
    }

    static const uint8_t llm[4] = { 192, 168, 1, 200 };

    kprint("\nTCP: connecting to ");
    kprint_ip(llm);
    kprint(":8087\n");

    int h = tcp_connect(llm, 8087, 6000);

    if (h < 0) {
        kprint("NET: TCP layer FAILED (connect)\n");
        return;
    }

    static const char request[] =
        "GET /health HTTP/1.0\r\n"
        "Host: 192.168.1.200\r\n"
        "Connection: close\r\n"
        "\r\n";

    uint32_t req_len = sizeof(request) - 1;

    int sent = tcp_send(h, request, req_len);

    kprint("TCP: sent ");
    kprint_dec(sent > 0 ? (uint32_t)sent : 0);
    kprint(" bytes\n");

    if (sent != (int)req_len) {
        tcp_close(h);
        kprint("NET: TCP layer FAILED (send)\n");
        return;
    }

    kprint("TCP: response (raw bytes):\n");
    kprint("-----\n");

    uint8_t buf[256];
    uint32_t total = 0;
    int end_state = 0;

    for (;;) {

        int n = tcp_recv(h, buf, sizeof(buf), 3000);

        if (n > 0) {

            for (int i = 0; i < n; ++i) {

                char ch = (char)buf[i];

                if (total + (uint32_t)i < 1024) {

                    if (ch == '\n' || (ch >= 32 && ch < 127)) {
                        kputc(ch);
                    } else if (ch != '\r') {
                        kputc('.');
                    }
                }
            }

            total += (uint32_t)n;
            continue;
        }

        end_state = n;
        break;
    }

    kprint("\n-----\n");
    kprint("TCP: received ");
    kprint_dec(total);
    kprint(" bytes, ");

    if (end_state == TCP_EOF) {
        kprint("peer closed (EOF)\n");
    } else if (end_state == TCP_ERR) {
        kprint("connection reset\n");
    } else {
        kprint("read timeout\n");
    }

    tcp_close(h);

    kprint("TCP: connection closed\n\n");

    if (total > 0) {
        kprint("NET: TCP layer OK\n");
    } else {
        kprint("NET: TCP layer FAILED (no data)\n");
    }
}
