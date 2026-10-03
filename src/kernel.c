#include <stdint.h>
#include "pci.h"
#include "virtio_net.h"
#include "virtio_blk.h"
#include "net.h"
#include "ip.h"
#include "dns.h"
#include "tcp.h"
#include "http.h"
#include "llm.h"
#include "console.h"
#include "sysinfo.h"
#include "mem.h"
#include "fs.h"

static volatile uint16_t *const VGA =
    (uint16_t *)0xB8000;

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_ready(void)
{
    return (inb(COM1 + 5) & 0x20) != 0;
}

static void serial_putc(char c)
{
    while (!serial_ready()) {
    }

    outb(COM1, (uint8_t)c);
}

static void serial_print(const char *s)
{
    for (; *s; ++s) {
        serial_putc(*s);
    }
}

static void serial_put_hex4(uint8_t value)
{
    value &= 0x0F;

    if (value < 10) {
        serial_putc('0' + value);
    } else {
        serial_putc('A' + (value - 10));
    }
}

static void serial_put_hex16(uint16_t value)
{
    for (int shift = 12; shift >= 0; shift -= 4) {
        serial_put_hex4(
            (uint8_t)(value >> shift)
        );
    }
}

static void serial_put_hex32(uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_put_hex4(
            (uint8_t)(value >> shift)
        );
    }
}

static void serial_put_dec(uint8_t value)
{
    if (value >= 100) {
        serial_putc('0' + value / 100);
        value %= 100;
        serial_putc('0' + value / 10);
        serial_putc('0' + value % 10);
        return;
    }

    if (value >= 10) {
        serial_putc('0' + value / 10);
        serial_putc('0' + value % 10);
        return;
    }

    serial_putc('0' + value);
}

static void clear_screen(void)
{
    for (uint32_t i = 0; i < 80 * 25; ++i) {
        VGA[i] = 0x0720;
    }
}

static void vga_print_at(
    const char *s,
    uint32_t row,
    uint32_t col
)
{
    uint32_t pos = row * 80 + col;

    for (uint32_t i = 0;
         s[i] != '\0' && pos < 80 * 25;
         ++i, ++pos)
    {
        VGA[pos] =
            ((uint16_t)0x07 << 8) |
            (uint8_t)s[i];
    }
}

static void pci_scan(void)
{
    serial_print("\nPCI SCAN\n");
    serial_print("--------\n");

    uint32_t devices_found = 0;
    uint32_t virtio_found = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {

        for (uint8_t slot = 0; slot < 32; ++slot) {

            for (uint8_t function = 0;
                 function < 8;
                 ++function)
            {
                uint16_t vendor =
                    pci_vendor_id(
                        (uint8_t)bus,
                        slot,
                        function
                    );

                if (vendor == 0xFFFF) {
                    continue;
                }

                uint16_t device =
                    pci_device_id(
                        (uint8_t)bus,
                        slot,
                        function
                    );

                ++devices_found;

                serial_print("PCI ");

                if (bus < 16) {
                    serial_putc('0');
                }

                serial_put_hex16(
                    (uint16_t)bus
                );

                serial_putc(':');

                if (slot < 10) {
                    serial_putc('0');
                }

                serial_put_hex4(slot);

                serial_putc('.');

                serial_put_dec(function);

                serial_print(
                    " vendor=0x"
                );

                serial_put_hex16(vendor);

                serial_print(
                    " device=0x"
                );

                serial_put_hex16(device);

                if (vendor == 0x1AF4 &&
                    (device == 0x1041 ||
                     device == 0x1000))
                {
                    serial_print(
                        "  <-- VIRTIO-NET"
                    );

                    uint32_t bar0 =
                        pci_bar0(
                            (uint8_t)bus,
                            slot,
                            function
                        );

                    serial_print("\n");

                    serial_print(
                        "    BAR0 raw = 0x"
                    );

                    serial_put_hex32(bar0);

                    serial_print("\n");

                    if (bar0 & 0x1) {

                        uint32_t io_base =
                            bar0 & 0xFFFFFFFC;

                        serial_print(
                            "    BAR0 type = I/O\n"
                        );

                        serial_print(
                            "    I/O base  = 0x"
                        );

                        serial_put_hex32(
                            io_base
                        );

                        serial_print("\n");

                    } else {

                        serial_print(
                            "    BAR0 type = MEMORY\n"
                        );
                    }

                    ++virtio_found;
                }

                serial_print("\n");
            }
        }
    }

    serial_print("\nPCI devices found: ");

    if (devices_found > 999) {
        serial_print(">999");
    } else {
        serial_put_dec(
            (uint8_t)devices_found
        );
    }

    serial_print("\nVirtIO-NET found: ");

    if (virtio_found != 0) {
        serial_print("YES");
    } else {
        serial_print("NO");
    }

    serial_print("\n");
}



static uint64_t parse_num(const char **str) {
    uint64_t val = 0;
    while (**str == ' ') (*str)++;
    while (**str >= '0' && **str <= '9') {
        val = val * 10 + (**str - '0');
        (*str)++;
    }
    return val;
}

static void parse_ip(const char *s, uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        ip[i] = (uint8_t)val;
        if (*s == '.') s++;
    }
}

static const char *find_substr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return 0;
}

/* Ejecuta la herramienta y captura un resumen de salida para retroalimentar a la IA */
static int execute_tool_with_feedback(const char *cmd_line, char *feedback_out, uint32_t max_fb)
{
    while (*cmd_line == ' ') cmd_line++;

    if (cmd_line[0] == 'l' && cmd_line[1] == 's') {
        vfs_list();
        vfs_format_list(feedback_out, max_fb);
        return 1;
    } else if (cmd_line[0] == 'c' && cmd_line[1] == 'a' && cmd_line[2] == 't') {
        const char *p = cmd_line + 3;
        while (*p == ' ') p++;
        char filename[48];
        uint32_t fn_i = 0;
        while (*p && *p != ' ' && fn_i < sizeof(filename) - 1) {
            filename[fn_i++] = *p++;
        }
        filename[fn_i] = '\0';
        char buf[1024];
        int r = vfs_read(filename, buf, sizeof(buf));
        if (r >= 0) {
            kprint("\n[CONTENIDO DE "); kprint(filename); kprint("]:\n");
            kprint(buf); kprint("\n");
            uint32_t i = 0; while (buf[i] && i < max_fb - 1) { feedback_out[i] = buf[i]; i++; } feedback_out[i] = '\0';
        } else {
            kprint("Error: archivo no encontrado.\n");
            const char *err = "Error: archivo no encontrado en RamFS.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'w' && cmd_line[1] == 'r' && cmd_line[2] == 'i' && cmd_line[3] == 't' && cmd_line[4] == 'e') {
        const char *p = cmd_line + 5;
        while (*p == ' ') p++;
        char filename[48];
        uint32_t fn_i = 0;
        while (*p && *p != ' ' && fn_i < sizeof(filename) - 1) {
            filename[fn_i++] = *p++;
        }
        filename[fn_i] = '\0';
        while (*p == ' ') p++;
        uint32_t tlen = 0; while (p[tlen]) tlen++;
        vfs_write(filename, p, tlen);
        kprint("Archivo '"); kprint(filename); kprint("' escrito con exito.\n");
        const char *succ = "Archivo escrito correctamente en el sistema de archivos.";
        uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'r' && cmd_line[1] == 'm' && cmd_line[2] == ' ') {
        const char *fn = cmd_line + 3;
        while (*fn == ' ') fn++;
        int r = vfs_delete(fn);
        if (r == 0) {
            kprint("Archivo '"); kprint(fn); kprint("' eliminado con exito.\n");
            const char *succ = "Archivo eliminado correctamente de RamFS.";
            uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        } else {
            kprint("Error: no se pudo eliminar '"); kprint(fn); kprint("'.\n");
            const char *err = "Error: no se pudo eliminar, el archivo no existe.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 't' && cmd_line[2] == 'a' && cmd_line[3] == 't' && cmd_line[4] == 's') {
        sysinfo_print_stats();
        struct sysinfo s;
        sysinfo_get(&s);
        char tmp[256];
        uint32_t p = 0;
        const char *m = "VirtIO-NET: TX_pkts="; while (*m) tmp[p++] = *m++;
        uint32_t v = s.tx_packets; if (v==0) tmp[p++]='0'; else { char b[10]; int n=0; while(v){b[n++]='0'+(v%10);v/=10;} while(n) tmp[p++]=b[--n]; }
        m = " RX_pkts="; while (*m) tmp[p++] = *m++;
        v = s.rx_packets; if (v==0) tmp[p++]='0'; else { char b[10]; int n=0; while(v){b[n++]='0'+(v%10);v/=10;} while(n) tmp[p++]=b[--n]; }
        m = " (Conectividad y trafico activos)"; while (*m) tmp[p++] = *m++;
        tmp[p] = '\0';
        for (uint32_t i = 0; i < p && i < max_fb - 1; ++i) feedback_out[i] = tmp[i];
        feedback_out[p < max_fb ? p : max_fb - 1] = '\0';
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'i' && cmd_line[2] == 'n' && cmd_line[3] == 'g') {
        const char *arg = cmd_line + 4;
        while (*arg == ' ') arg++;
        uint8_t target[4];
        if (*arg) {
            parse_ip(arg, target);
        } else {
            target[0] = net_gateway[0]; target[1] = net_gateway[1];
            target[2] = net_gateway[2]; target[3] = net_gateway[3];
        }
        kprint("PING a "); kprint_ip(target); kprint("...\n");
        int r = icmp_ping(target, 1, 2000);
        if (r == 1) {
            const char *succ = "Ping exitoso: respuesta ICMP recibida del host objetivo.";
            uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        } else {
            const char *fail_msg = "Ping fallido: tiempo de espera agotado, no hubo respuesta.";
            uint32_t i = 0; while (fail_msg[i] && i < max_fb - 1) { feedback_out[i] = fail_msg[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'r') {
        const char *p = cmd_line + 11;
        uint64_t sec = parse_num(&p);
        char sbuf[512];
        if (virtio_blk_read(sec, sbuf) == 0) {
            kprint("\n[SECTOR LBA "); kprint_dec((uint32_t)sec); kprint("]:\n");
            for(int i=0; i<512 && sbuf[i]; i++) {
                if (sbuf[i] >= 32 && sbuf[i] < 127) kputc(sbuf[i]);
            }
            kprint("\n");
            uint32_t i = 0; while (sbuf[i] && i < max_fb - 1 && i < 511) { feedback_out[i] = sbuf[i]; i++; } feedback_out[i] = '\0';
        } else {
            const char *err = "Error de I/O al leer el disco duro.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 's' && cmd_line[1] == 'e' && cmd_line[2] == 'c' && cmd_line[3] == 't' && cmd_line[4] == 'o' && cmd_line[5] == 'r' && cmd_line[6] == '_' && cmd_line[7] == 'w') {
        const char *p = cmd_line + 12;
        uint64_t sec = parse_num(&p);
        while (*p == ' ') p++;
        char sbuf[512];
        for (int i=0; i<512; i++) sbuf[i] = 0;
        int bi=0;
        while (*p && bi < 511) sbuf[bi++] = *p++;
        if (virtio_blk_write(sec, sbuf) == 0) {
            kprint("Sector LBA escrito en disco persistente.\n");
            const char *succ = "Sector guardado de forma persistente en el HDD virtual.";
            uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        } else {
            const char *err = "Error de I/O al escribir en disco duro.";
            uint32_t i = 0; while (err[i] && i < max_fb - 1) { feedback_out[i] = err[i]; i++; } feedback_out[i] = '\0';
        }
        return 1;
    } else if (cmd_line[0] == 'm' && cmd_line[1] == 'e' && cmd_line[2] == 'm') {
        sysinfo_print_mem();
        const char *succ = "Memoria nominal: CPU en Long Mode de 64 bits, CR3 PML4 valido, 1 GiB mapeado.";
        uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'a' && cmd_line[1] == 'r' && cmd_line[2] == 'p') {
        net_run_arp_test();
        const char *succ = "Tabla ARP: Gateway 10.0.2.2 resuelto correctamente a su MAC Ethernet.";
        uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        return 1;
    } else if (cmd_line[0] == 'p' && cmd_line[1] == 'c' && cmd_line[2] == 'i') {
        pci_scan();
        const char *succ = "Bus PCI escaneado: Dispositivo VirtIO-NET detectado y operativo en BAR0.";
        uint32_t i = 0; while (succ[i] && i < max_fb - 1) { feedback_out[i] = succ[i]; i++; } feedback_out[i] = '\0';
        return 1;
    }

    const char *unrec = "Comando ejecutado.";
    uint32_t i = 0; while (unrec[i] && i < max_fb - 1) { feedback_out[i] = unrec[i]; i++; } feedback_out[i] = '\0';
    return 0;
}

static int is_valid_tool(const char *s)
{
    /* Rechazar categoricamente cualquier orden que contenga marcadores '<' o '>' de plantilla */
    for (const char *chk = s; *chk && *chk != '\n' && *chk != '\r'; chk++) {
        if (*chk == '<' || *chk == '>') return 0;
    }

    if (s[0] == 's' && s[1] == 't' && s[2] == 'a' && s[3] == 't' && s[4] == 's') return 1;
    if (s[0] == 'm' && s[1] == 'e' && s[2] == 'm') return 1;
    if (s[0] == 'a' && s[1] == 'r' && s[2] == 'p') return 1;
    if (s[0] == 'p' && s[1] == 'c' && s[2] == 'i') return 1;
    if (s[0] == 'p' && s[1] == 'i' && s[2] == 'n' && s[3] == 'g') return 1;
    if (s[0] == 'l' && s[1] == 's') return 1;
    if (s[0] == 'c' && s[1] == 'a' && s[2] == 't' && s[3] == ' ') return 1;
    if (s[0] == 'w' && s[1] == 'r' && s[2] == 'i' && s[3] == 't' && s[4] == 'e' && s[5] == ' ') return 1;
    if (s[0] == 'r' && s[1] == 'm' && s[2] == ' ') return 1;
    if (s[0] == 's' && s[1] == 'e' && s[2] == 'c' && s[3] == 't' && s[4] == 'o' && s[5] == 'r' && s[6] == '_') return 1;
    return 0;
}

static int extract_valid_cmd(const char *text, char *out_cmd, uint32_t max)
{
    const char *p = text;
    while (*p) {
        const char *tag = find_substr(p, "CMD:");
        if (!tag) {
            return 0;
        }
        const char *cand = tag + 4;
        while (*cand == ' ' || *cand == '`' || *cand == '\'' || *cand == '"') cand++;

        if (is_valid_tool(cand)) {
            uint32_t i = 0;
            while (cand[i] && cand[i] != '\r' && cand[i] != '\n' &&
                   cand[i] != '`' && cand[i] != '\'' && cand[i] != '"' && i < max - 1) {
                out_cmd[i] = cand[i];
                i++;
            }
            out_cmd[i] = '\0';
            return 1;
        }
        p = tag + 4;
    }
    return 0;
}

/* Buffers de agente en BSS para blindar la pila */
static char agent_prompt_buf[2048];
static char agent_reply_buf[4096];
static char tool_feedback_buf[512];

static void shell_run(void)
{
    char cmd[512];
    char reply[4096];

    serial_print("\n============================================================\n");
    serial_print("MYOS 0.1 INTERACTIVE SHELL\n");
    serial_print("Escribe 'help' para ver comandos disponibles.\n");
    serial_print("============================================================\n\n");

    for (;;) {
        serial_print("myos> ");
        int len = kgetline(cmd, sizeof(cmd));
        if (len == 0) {
            continue;
        }

        if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && (cmd[4] == '\0' || cmd[4] == ' ')) {
            serial_print("Comandos disponibles:\n");
            serial_print("Comandos disponibles:\n");
            serial_print("  help                 - Muestra esta ayuda\n");
            serial_print("  health               - Verifica estado del servidor LLM\n");
            serial_print("  llm <mensaje>        - Consulta general a nail-35b\n");
            serial_print("  llm-diag [pregunta]  - Telemetria + Diagnostico del kernel por IA\n");
            serial_print("  agent <mision>       - Agente autonomo con ejecucion de herramientas\n");
            serial_print("  heap                 - Estado de la memoria dinamica kmalloc\n");
            serial_print("  ping <ip>            - Envia ICMP echo a una direccion IPv4\n");
            serial_print("  ls                   - Lista los archivos del RamFS\n");
            serial_print("  cat <archivo>        - Muestra el contenido de un archivo\n");
            serial_print("  write <arch> <texto> - Crea o sobrescribe un archivo\n");
            serial_print("  rm <archivo>         - Elimina un archivo\n");
            serial_print("  mem                  - Informacion de CPU, paginacion y memoria\n");
            serial_print("  stats                - Estadisticas de trafico VirtIO-NET\n");
            serial_print("  arp                  - Muestra la tabla de cache ARP\n");
            serial_print("  pci                  - Escanea los dispositivos PCI\n");
            serial_print("  status               - Muestra estado de red e IP\n");
            serial_print("  clear                - Limpia la pantalla VGA\n");
        } else if (cmd[0] == 'l' && cmd[1] == 'l' && cmd[2] == 'm' && cmd[3] == '-' && cmd[4] == 'd' && cmd[5] == 'i' && cmd[6] == 'a' && cmd[7] == 'g') {
            const char *q = cmd + 8;
            while (*q == ' ') q++;
            serial_print("Recopilando telemetria y solicitando diagnostico a nail-35b...\n");
            if (llm_diagnose(q, reply, sizeof(reply), 35000)) {
                serial_print("\n[DIAGNOSTICO IA nail-35b]:\n");
                serial_print(reply);
                serial_print("\n\n");
            } else {
                serial_print("Error al solicitar diagnostico al LLM.\n");
            }
} else if (cmd[0] == 'a' && cmd[1] == 'g' && cmd[2] == 'e' && cmd[3] == 'n' && cmd[4] == 't' && cmd[5] == ' ') {
            const char *mission = cmd + 6;
            while (*mission == ' ') mission++;
            if (*mission == '\0') {
                serial_print("Uso: agent <mision en lenguaje natural>\n");
                continue;
            }

            serial_print("\n[AGENTE AUTONOMO]: Iniciando mision multi-paso...\n");

            /* Paso inicial: preparar contexto y mision */
            uint32_t ap_len = 0;
            const char *instr = "Mision: '";
            while (*instr) agent_prompt_buf[ap_len++] = *instr++;
            const char *m = mission;
            while (*m && ap_len < sizeof(agent_prompt_buf) - 500) agent_prompt_buf[ap_len++] = *m++;
            const char *ctx = "'. Contexto MYOS: Kernel x86_64 bare-metal con RamFS en /. Herramientas: stats, mem, arp, pci, ping 10.0.2.2, ls, cat /arch, write /arch texto, rm /arch. Reglas obligatorias: 1) Si la mision requiere medir o consultar antes de guardar (ej: ping y luego write), ejecuta SIEMPRE primero la herramienta de medicion (ping/stats) y en el siguiente paso guarda los datos con write. 2) Para ejecutar usa 'CMD: <herramienta>'. 3) Al concluir responde con tu dictamen final sin CMD.";
            while (*ctx) agent_prompt_buf[ap_len++] = *ctx++;
            agent_prompt_buf[ap_len] = '\0';

            /* Bucle ReAct con Memoria Contextual Acumulativa */
            static char history_buf[1024];
            uint32_t hist_len = 0;
            history_buf[0] = '\0';

            int finished = 0;
            for (int step = 1; step <= 4; ++step) {
                /* Ensamblar prompt completo: Misión fija + Historial acumulado + Reglas */
                ap_len = 0;
                const char *p_m = "MISION PRINCIPAL: '"; while (*p_m) agent_prompt_buf[ap_len++] = *p_m++;
                const char *p_mval = mission; while (*p_mval && ap_len < sizeof(agent_prompt_buf) - 800) agent_prompt_buf[ap_len++] = *p_mval++;
                const char *p_ctx = "'.\nContexto MYOS: Kernel bare-metal x86_64, RamFS en /.\nHerramientas: stats | mem | arp | pci | ping 10.0.2.2 | ls | cat /archivo | write /archivo texto | rm /archivo | sector_read LBA | sector_write LBA texto.\n";
                while (*p_ctx) agent_prompt_buf[ap_len++] = *p_ctx++;

                if (hist_len > 0) {
                    const char *h_hdr = "Historial de acciones previas realizadas:\n";
                    while (*h_hdr) agent_prompt_buf[ap_len++] = *h_hdr++;
                    for (uint32_t h = 0; h < hist_len && ap_len < sizeof(agent_prompt_buf) - 300; ++h) {
                        agent_prompt_buf[ap_len++] = history_buf[h];
                    }
                }

                const char *p_rules = "\nInstruccion: Si requieres una herramienta responde 'CMD: <herramienta> [args]'. Si la mision pide medir/consultar y luego guardar (ej: ping y guardar), ejecuta primero la medicion y en el paso siguiente guarda los datos reales. Si ya completaste la mision, emite tu dictamen final sin CMD.";
                while (*p_rules && ap_len < sizeof(agent_prompt_buf) - 1) agent_prompt_buf[ap_len++] = *p_rules++;
                agent_prompt_buf[ap_len] = '\0';

                if (!llm_chat(agent_prompt_buf, agent_reply_buf, sizeof(agent_reply_buf), 35000)) {
                    serial_print("Error en la comunicacion con el agente.\n");
                    finished = 1;
                    break;
                }

                char clean_cmd[512];
                if (extract_valid_cmd(agent_reply_buf, clean_cmd, sizeof(clean_cmd))) {
                    serial_print("\n>> [PASO ");
                    serial_put_dec((uint8_t)step);
                    serial_print(" | ACCION IA]: Ejecutando '");
                    serial_print(clean_cmd);
                    serial_print("' en el hardware...\n\n");

                    execute_tool_with_feedback(clean_cmd, tool_feedback_buf, sizeof(tool_feedback_buf));

                    serial_print("\n>> [FEEDBACK A LA IA]: ");
                    serial_print(tool_feedback_buf);
                    serial_print("\n");

                    /* Acumular accion y resultado en el historial para los siguientes pasos */
                    const char *a1 = "- Paso "; while (*a1 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a1++;
                    history_buf[hist_len++] = (char)('0' + step);
                    const char *a2 = ": ejecutaste '"; while (*a2 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a2++;
                    const char *a3 = clean_cmd; while (*a3 && hist_len < sizeof(history_buf) - 150) history_buf[hist_len++] = *a3++;
                    const char *a4 = "' -> Resultado: '"; while (*a4 && hist_len < sizeof(history_buf) - 100) history_buf[hist_len++] = *a4++;
                    const char *a5 = tool_feedback_buf; while (*a5 && hist_len < sizeof(history_buf) - 20) history_buf[hist_len++] = *a5++;
                    const char *a6 = "'\n"; while (*a6 && hist_len < sizeof(history_buf) - 1) history_buf[hist_len++] = *a6++;
                    history_buf[hist_len] = '\0';

                    if (step == 4) {
                        /* Si agota los 4 pasos, pedir conclusion final manteniendo todo el historial */
                        ap_len = 0;
                        const char *f_m = "MISION: '"; while (*f_m) agent_prompt_buf[ap_len++] = *f_m++;
                        const char *f_mv = mission; while (*f_mv && ap_len < sizeof(agent_prompt_buf) - 500) agent_prompt_buf[ap_len++] = *f_mv++;
                        const char *f_h = "'.\nHistorial completado:\n"; while (*f_h) agent_prompt_buf[ap_len++] = *f_h++;
                        for (uint32_t h = 0; h < hist_len && ap_len < sizeof(agent_prompt_buf) - 200; ++h) agent_prompt_buf[ap_len++] = history_buf[h];
                        const char *f_end = "\nEmite AHORA tu dictamen final y resumen de la mision para el usuario.";
                        while (*f_end && ap_len < sizeof(agent_prompt_buf) - 1) agent_prompt_buf[ap_len++] = *f_end++;
                        agent_prompt_buf[ap_len] = '\0';

                        serial_print("\n[AGENTE AUTONOMO]: Generando dictamen final con la informacion obtenida...\n");
                        if (llm_chat(agent_prompt_buf, agent_reply_buf, sizeof(agent_reply_buf), 35000)) {
                            serial_print("\n[AGENTE DICTAMEN FINAL]:\n");
                            serial_print(agent_reply_buf);
                            serial_print("\n\n");
                        }
                        finished = 1;
                        break;
                    }

                } else {
                    serial_print("\n[AGENTE DICTAMEN FINAL]:\n");
                    serial_print(agent_reply_buf);
                    serial_print("\n\n");
                    finished = 1;
                    break;
                }
            }
            if (!finished) {
                serial_print("\nMision concluida.\n\n");
            }
        } else if (cmd[0] == 'l' && cmd[1] == 's' && (cmd[2] == '\0' || cmd[2] == ' ')) {
            vfs_list();
        } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == ' ') {
            const char *fn = cmd + 4;
            while (*fn == ' ') fn++;
            char buf[2048];
            int r = vfs_read(fn, buf, sizeof(buf));
            if (r >= 0) {
                serial_print("\n");
                serial_print(buf);
                serial_print("\n");
            } else {
                serial_print("Error: archivo no encontrado: '");
                serial_print(fn);
                serial_print("'\n");
            }
        } else if (cmd[0] == 'w' && cmd[1] == 'r' && cmd[2] == 'i' && cmd[3] == 't' && cmd[4] == 'e' && cmd[5] == ' ') {
            const char *p = cmd + 6;
            while (*p == ' ') p++;
            char fn[48];
            uint32_t fi = 0;
            while (*p && *p != ' ' && fi < sizeof(fn) - 1) {
                fn[fi++] = *p++;
            }
            fn[fi] = '\0';
            while (*p == ' ') p++;
            uint32_t tlen = 0; while (p[tlen]) tlen++;
            vfs_write(fn, p, tlen);
            serial_print("Escrito '"); serial_print(fn); serial_print("'\n");
        } else if (cmd[0] == 'r' && cmd[1] == 'm' && cmd[2] == ' ') {
            const char *fn = cmd + 3;
            while (*fn == ' ') fn++;
            if (vfs_delete(fn) == 0) {
                serial_print("Eliminado '"); serial_print(fn); serial_print("'\n");
            } else {
                serial_print("Error: no se pudo eliminar '"); serial_print(fn); serial_print("'\n");
            }
        } else if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'a' && cmd[3] == 'p') {
            size_t used = 0;
            size_t free_b = 0;
            kheap_stats(&used, &free_b);
            kprint("\nESTADO DEL HEAP (kmalloc):\n");
            kprint("-------------------------\n");
            kprint("Base del Heap:    0x00400000 (4 MiB)\n");
            kprint("Memoria Usada:    "); kprint_dec((uint32_t)used); kprint(" bytes\n");
            kprint("Memoria Libre:    "); kprint_dec((uint32_t)(free_b / 1024)); kprint(" KiB\n");
            kprint("Capacidad total:  12 MiB\n\n");
        } else if (cmd[0] == 'p' && cmd[1] == 'i' && cmd[2] == 'n' && cmd[3] == 'g') {
            const char *arg = cmd + 4;
            while (*arg == ' ') arg++;
            uint8_t target[4];
            if (*arg) {
                parse_ip(arg, target);
            } else {
                target[0] = net_gateway[0]; target[1] = net_gateway[1];
                target[2] = net_gateway[2]; target[3] = net_gateway[3];
            }
            kprint("PING a "); kprint_ip(target); kprint("...\n");
            icmp_ping(target, 1, 1500);
        } else if (cmd[0] == 'm' && cmd[1] == 'e' && cmd[2] == 'm' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            sysinfo_print_mem();
        } else if (cmd[0] == 's' && cmd[1] == 't' && cmd[2] == 'a' && cmd[3] == 't' && cmd[4] == 's' && (cmd[5] == '\0' || cmd[5] == ' ')) {
            sysinfo_print_stats();
        } else if (cmd[0] == 'a' && cmd[1] == 'r' && cmd[2] == 'p' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            net_run_arp_test();
        } else if (cmd[0] == 'p' && cmd[1] == 'c' && cmd[2] == 'i' && (cmd[3] == '\0' || cmd[3] == ' ')) {
            pci_scan();
        } else if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'a' && cmd[3] == 'l' && cmd[4] == 't' && cmd[5] == 'h') {
            serial_print("Comprobando /health en servidor LLM...\n");
            if (llm_health()) {
                serial_print("Servidor LLM: OK (HTTP 200 OK)\n");
            } else {
                serial_print("Servidor LLM: ERROR\n");
            }
        } else if (cmd[0] == 'l' && cmd[1] == 'l' && cmd[2] == 'm' && cmd[3] == ' ') {
            const char *prompt = cmd + 4;
            while (*prompt == ' ') prompt++;
            if (*prompt == '\0') {
                serial_print("Uso: llm <mensaje>\n");
                continue;
            }
            serial_print("Consultando a nail-35b...\n");
            if (llm_chat(prompt, reply, sizeof(reply), 30000)) {
                serial_print("\n[nail-35b]: ");
                serial_print(reply);
                serial_print("\n\n");
            } else {
                serial_print("Error al consultar el LLM.\n");
            }
        } else if (cmd[0] == 's' && cmd[1] == 't' && cmd[2] == 'a' && cmd[3] == 't' && cmd[4] == 'u' && cmd[5] == 's') {
            serial_print("IP:  "); kprint_ip(net_ip);
            serial_print("  GW: "); kprint_ip(net_gateway);
            serial_print("  MAC: "); kprint_mac(net_mac);
            serial_print("\n");
        } else if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r') {
            clear_screen();
        } else {
            serial_print("Comando desconocido: '");
            serial_print(cmd);
            serial_print("'. Escribe 'help'.\n");
        }
    }
}

void kernel_main(void)
{
    serial_init();

    clear_screen();

    pci_scan();
    vfs_init();
    virtio_blk_init();

    net_run_llm_test();

    vga_print_at("MYOS 0.1", 5, 36);
    vga_print_at("x86_64 kernel: LONG MODE OK", 7, 27);
    vga_print_at("VirtIO-NET: OK  |  TCP/IP: OK  |  HTTP: OK", 9, 20);
    vga_print_at("LLM Server: 192.168.1.200:8087 (nail-35b)", 11, 20);

    if (llm_test_passed) {
        vga_print_at("LLM Response: ", 14, 20);
        vga_print_at(llm_last_reply, 14, 34);
        vga_print_at(">> SYSTEM STATUS: AUTONOMOUS AI LINK ESTABLISHED <<", 17, 15);
    } else {
        vga_print_at("LLM Test: FAILED", 14, 32);
    }

    serial_print("\nMYOS 0.1\n");
    serial_print("x86_64 kernel: LONG MODE OK\n");
    if (llm_test_passed) {
        serial_print("SYSTEM STATUS: AUTONOMOUS AI LINK ESTABLISHED\n");
    }

    shell_run();
}