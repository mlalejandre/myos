#include "arch.h"
#include "agent.h"
#include "console.h"
#include "llm.h"
#include "fs.h"
#include "virtio_blk.h"
#include "shell.h"
#include "mem.h"
#include "io.h"

#define RESULT_LBA          20
#define RESUME_FLAG_LBA     25
#define AGENT_STATE_FILE    "/agent/state"
#define AGENT_MAX_PATCHES   3
#define AGENT_HIST_KEEP     1800
#define AGENT_HIST_MARK     "\n@@HISTORY@@\n"

#define MAILBOX_SIZE        8192
#define MAILBOX_LBA         1
#define MAILBOX_HDR         12

static char history_buf[4096];
static char agent_state_buf[4096];
char agent_resume_mission[512];
int  agent_resume_pending = 0;
static int agent_patch_attempts = 0;

static char agent_prompt_buf[8192];
static char agent_reply_buf[4096];
static char tool_feedback_buf[2048];
static char mailbox_buf[MAILBOX_SIZE];
static char hpatch_buf[2048];

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

static void fb_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    if (!out || max == 0 || !pos || *pos >= max - 1) return;
    while (*s && *pos + 1 < max) {
        out[(*pos)++] = *s++;
    }
    out[*pos] = '\0';
}

static void fb_putc(char *out, uint32_t max, uint32_t *pos, char c)
{
    if (!out || max == 0) return;
    if (*pos + 1 < max) {
        out[(*pos)++] = c;
        out[*pos] = '\0';
    }
}

static void fb_put_dec(char *out, uint32_t max, uint32_t *pos, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) { fb_putc(out, max, pos, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) fb_putc(out, max, pos, tmp[--n]);
}

static int is_patch_target_protected(const char *name)
{
    static const char *const protected_list[] = {
        "boot.s", "idt.c", "idt.h", "io.h", "srcfs.c", "srcfs.h",
        "virtio_blk.c", "virtio_blk.h", "boot_gate.c", "boot_gate.h",
        "pmm.c", "pmm.h", "vmm.c", "vmm.h", "mem.c", "mem.h",
        "thread.c", "thread.h", "switch.s", "mutex.c", "mutex.h", 0
    };
    while (*name == ' ' || *name == '\t') name++;
    if (name[0] == '/') name++;
    if (name[0] == 's' && name[1] == 'r' && name[2] == 'c' && name[3] == '/') name += 4;

    for (int i = 0; protected_list[i]; ++i) {
        const char *a = name;
        const char *b = protected_list[i];
        while (*a && *b && (*a == *b)) { a++; b++; }
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\n' || *a == '\r' || *a == '"')) {
            return 1;
        }
    }
    return 0;
}

static void host_submit_patch(const char *text)
{
    uint32_t n = 0;
    while (text[n]) n++;

    if (n == 0 || n > MAILBOX_SIZE - MAILBOX_HDR) {
        kprint("\n[PARCHE] Rechazado: tamano invalido.\n");
        return;
    }

    memset(mailbox_buf, 0, sizeof(mailbox_buf));
    memcpy(mailbox_buf, "PATCHv02", 8);
    memcpy(mailbox_buf + 8, &n, 4);
    memcpy(mailbox_buf + MAILBOX_HDR, text, n);

    for (int s = 0; s < MAILBOX_SIZE / 512; ++s) {
        if (virtio_blk_write(MAILBOX_LBA + s, mailbox_buf + s * 512) != 0) {
            kprint("\n[PARCHE] Error de I/O escribiendo el buzon.\n");
            return;
        }
    }

    kprint("\n========================================================================\n");
    kprint("!!! PUENTE HOST-BRIDGE: PARCHE ENVIADO, SOLICITANDO RECOMPILACION !!!\n");
    kprint("========================================================================\n");

    qemu_exit(0x10);
    for (;;) { arch_interrupts_disable(); arch_halt(); }
}

static void agent_state_save(const char *mission, const char *history, uint32_t hist_len)
{
    static uint8_t zero_sec[512];
    uint32_t pos = 0;
    uint32_t start = hist_len > AGENT_HIST_KEEP ? hist_len - AGENT_HIST_KEEP : 0;

    if (start > 0) {
        const char *nx = find_substr(history + start, "\n- Paso ");
        start = nx ? (uint32_t)(nx + 1 - history) : hist_len;
    }

    agent_state_buf[0] = '\0';
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "ATTEMPTS:");
    fb_put_dec(agent_state_buf, sizeof(agent_state_buf), &pos, (uint32_t)(agent_patch_attempts + 1));
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "\n");
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, mission);
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, AGENT_HIST_MARK);
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, history + start);

    vfs_write(AGENT_STATE_FILE, agent_state_buf, pos);
    virtio_blk_write(RESULT_LBA, zero_sec);
}

void agent_state_load(void)
{
    static uint8_t sec[512];
    static uint8_t res[512];

    if (virtio_blk_read(RESUME_FLAG_LBA, sec) != 0 || memcmp(sec, "RESUME_REQ", 10) != 0) {
        return;
    }

    memset(sec, 0, sizeof(sec));
    virtio_blk_write(RESUME_FLAG_LBA, sec);

    if (vfs_read(AGENT_STATE_FILE, agent_state_buf, sizeof(agent_state_buf)) <= 0) {
        return;
    }
    vfs_delete(AGENT_STATE_FILE);

    const char *p = agent_state_buf;
    if (memcmp(p, "ATTEMPTS:", 9) != 0) return;
    p += 9;

    int n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    if (*p == '\n') p++;

    const char *hm = find_substr(p, AGENT_HIST_MARK);
    if (!hm) return;

    uint32_t m = 0;
    while (p + m < hm && m < sizeof(agent_resume_mission) - 1) {
        agent_resume_mission[m] = p[m];
        m++;
    }
    agent_resume_mission[m] = '\0';
    if (m == 0) return;

    uint32_t pos = 0;
    history_buf[0] = '\0';
    fb_puts(history_buf, sizeof(history_buf), &pos, hm + (sizeof(AGENT_HIST_MARK) - 1));
    if (pos > 0 && history_buf[pos - 1] != '\n') fb_putc(history_buf, sizeof(history_buf), &pos, '\n');

    if (virtio_blk_read(RESULT_LBA, res) == 0 && res[0] != 0) {
        res[511] = 0;
        for (int i = 0; res[i]; ++i) if (res[i] < 32 || res[i] > 126) res[i] = ' ';
    } else {
        res[0] = '\0';
    }

    if (find_substr((const char *)res, "BOOT_OK")) {
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "- Paso R: El host APROBO y COMPILO tu parche con exito (BOOT_OK).\n"
                "  [DIRECTIVA]: Tu parche ya esta aplicado. NO envies mas parches. Concluye con action=\"final\".\n");
    } else if (find_substr((const char *)res, "FAILED") || find_substr((const char *)res, "REJECTED")) {
        fb_puts(history_buf, sizeof(history_buf), &pos,
                "- Paso R: El parche NO fue aceptado (rollback aplicado).\n  [DETALLE]: ");
        fb_puts(history_buf, sizeof(history_buf), &pos, (const char *)res);
        fb_puts(history_buf, sizeof(history_buf), &pos, "\n  [DIRECTIVA]: Corrige tu SEARCH/REPLACE y reintenta.\n");
    }

    agent_patch_attempts = n;
    agent_resume_pending = 1;
}

static void inject_agent_memory(char *dst, uint32_t max, uint32_t *pos)
{
    static const char *const mem_files[] = {
        "/etc/mem_user.txt", "/etc/mem_hw.txt", "/etc/mem_kernel.txt"
    };
    static const char *const mem_labels[] = {
        "- Usuario/Identidad: ", "- Hardware/Red: ", "- Codigo/Kernel: "
    };

    int any = 0;
    char buf[384];

    for (int i = 0; i < 3; ++i) {
        int n = vfs_read(mem_files[i], buf, sizeof(buf));
        if (n > 0) {
            if (!any) {
                fb_puts(dst, max, pos, "\n[MEMORIA PERMANENTE APRENDIDA]:\n");
                any = 1;
            }
            fb_puts(dst, max, pos, mem_labels[i]);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
            fb_puts(dst, max, pos, buf);
            fb_puts(dst, max, pos, "\n");
        }
    }
}

static void history_add_note(uint32_t *hist_len, int step, const char *msg)
{
    if (!hist_len || *hist_len >= sizeof(history_buf) - 400) return;
    uint32_t pos = *hist_len;
    fb_puts(history_buf, sizeof(history_buf), &pos, "- Paso ");
    fb_putc(history_buf, sizeof(history_buf), &pos, (char)('0' + step));
    fb_puts(history_buf, sizeof(history_buf), &pos, ": ");
    fb_puts(history_buf, sizeof(history_buf), &pos, msg);
    fb_puts(history_buf, sizeof(history_buf), &pos, "\n");
    *hist_len = pos;
}

static int json_get2(const char *json, const char *k1, const char *k2, char *out, uint32_t max)
{
    int n = llm_json_get(json, k1, out, max);
    if (n <= 0) n = llm_json_get(json, k2, out, max);
    return n;
}

void agent_run(const char *mission)
{
    while (*mission == ' ') mission++;
    if (*mission == '\0') {
        kprint("Uso: soma <mision en lenguaje natural>\n");
        return;
    }

    kprint("\n[AGENTE AUTONOMO]: Iniciando mision multi-paso...\n");

    uint32_t hist_len = 0;
    if (agent_resume_pending == 1) {
        agent_resume_pending = 0;
        while (history_buf[hist_len]) hist_len++;
    } else {
        history_buf[0] = '\0';
    }

    uint32_t ap_len = 0;
    int finished = 0;

    for (int step = 1; step <= 8; ++step) {
        ap_len = 0;
        const char *p_m = "MISION: '"; while (*p_m) agent_prompt_buf[ap_len++] = *p_m++;
        const char *p_mval = mission; while (*p_mval && ap_len < sizeof(agent_prompt_buf) - 1200) agent_prompt_buf[ap_len++] = *p_mval++;
        const char *p_ctx = "'.\nContexto SOMA: Sistema Operativo Multi-Agente (kernel bare-metal x86_64). Red: IP 10.0.2.15, GW 10.0.2.2. RamFS en /.\n"
                            "Responde SIEMPRE con este esquema JSON:\n"
                            "{\n  \"thought\": \"analisis breve\",\n  \"action\": \"tool\" | \"patch\" | \"final\",\n"
                            "  \"cmd\": \"herramienta\",\n  \"patch\": {\"file\": \"src/archivo.c\", \"search\": \"exacto\", \"replace\": \"nuevo\"},\n"
                            "  \"verdict\": \"dictamen final\"\n}\n"
                            "Reglas:\n"
                            "1) Modificacion de codigo: boot, idt, pmm, vmm, mem, thread, mutex, virtio_blk, boot_gate estan PROTEGIDOS. Usa action=\"patch\".\n"
                            "2) Si el host aprobo tu parche (BOOT_OK), concluye directamente con action=\"final\".\n"
                            "3) Si la mision pide crear/guardar archivos, ejecuta write antes de emitir final.\n";
        while (*p_ctx) agent_prompt_buf[ap_len++] = *p_ctx++;

        inject_agent_memory(agent_prompt_buf, sizeof(agent_prompt_buf), &ap_len);

        if (hist_len > 0) {
            const char *h_hdr = "Acciones ya realizadas anteriormente:\n";
            while (*h_hdr) agent_prompt_buf[ap_len++] = *h_hdr++;
            for (uint32_t h = 0; h < hist_len && ap_len < sizeof(agent_prompt_buf) - 400; ++h) {
                agent_prompt_buf[ap_len++] = history_buf[h];
            }
        }

        const char *p_rules = "\nInstruccion: Analiza el historial y genera el JSON.";
        while (*p_rules && ap_len < sizeof(agent_prompt_buf) - 1) agent_prompt_buf[ap_len++] = *p_rules++;
        agent_prompt_buf[ap_len] = '\0';

        if (!llm_chat_json(agent_prompt_buf, agent_reply_buf, sizeof(agent_reply_buf), 40000)) {
            kprint("Error en comunicacion estructurada con el agente.\n");
            finished = 1;
            break;
        }

        static char act[32], th[1024], cmd_f[2048], patch_f[4096], verdict[4096];
        act[0] = '\0'; th[0] = '\0'; cmd_f[0] = '\0'; patch_f[0] = '\0'; verdict[0] = '\0';

        llm_json_get(agent_reply_buf, "action", act, sizeof(act));
        llm_json_get(agent_reply_buf, "thought", th, sizeof(th));
        llm_json_get(agent_reply_buf, "cmd", cmd_f, sizeof(cmd_f));
        llm_json_get(agent_reply_buf, "patch", patch_f, sizeof(patch_f));
        llm_json_get(agent_reply_buf, "verdict", verdict, sizeof(verdict));

        if (th[0]) {
            kprint("\n>> [PASO ");
            kprint_dec((uint32_t)step);
            kprint(" | ANALISIS]: ");
            kprint(th);
            kprint("\n");
        }

        if (find_substr(act, "patch") && patch_f[0] == '\0') {
            static char pf[96], ps[2048], pr[2048];
            pf[0] = '\0'; ps[0] = '\0'; pr[0] = '\0';
            json_get2(agent_reply_buf, "file", "FILE", pf, sizeof(pf));
            json_get2(agent_reply_buf, "search", "SEARCH", ps, sizeof(ps));
            json_get2(agent_reply_buf, "replace", "REPLACE", pr, sizeof(pr));

            if (pf[0] && ps[0]) {
                const char *pfp = pf;
                uint32_t pp = 0;
                while (*pfp == '/') pfp++;
                fb_puts(patch_f, sizeof(patch_f), &pp, "FILE: ");
                if (!(pfp[0] == 's' && pfp[1] == 'r' && pfp[2] == 'c' && pfp[3] == '/')) fb_puts(patch_f, sizeof(patch_f), &pp, "src/");
                fb_puts(patch_f, sizeof(patch_f), &pp, pfp);
                fb_puts(patch_f, sizeof(patch_f), &pp, "\n<<<<<<< SEARCH\n");
                fb_puts(patch_f, sizeof(patch_f), &pp, ps);
                fb_puts(patch_f, sizeof(patch_f), &pp, "\n=======\n");
                fb_puts(patch_f, sizeof(patch_f), &pp, pr);
                fb_puts(patch_f, sizeof(patch_f), &pp, "\n>>>>>>> REPLACE\n");
            }
        }

        if (find_substr(act, "patch") || patch_f[0] != '\0') {
            const char *ptext = patch_f[0] ? patch_f : agent_reply_buf;
            const char *pfile = find_substr(ptext, "FILE: src/");
            if (pfile) {
                const char *target_fname = pfile + 10;
                if (is_patch_target_protected(target_fname)) {
                    kprint("\n[SEGURIDAD KERNEL] Modificacion denegada: Archivo del core protegido.\n");
                    history_add_note(&hist_len, step, "tu parche fue rechazado: los subsistemas del core estan PROTEGIDOS.");
                    continue;
                }
                kprint("\n[AGENTE AUTONOMO]: Parche estructurado emitido. Enviando al host...\n");
                if (agent_patch_attempts >= AGENT_MAX_PATCHES) {
                    kprint("\n[AGENTE AUTONOMO]: limite de parches por mision alcanzado.\n");
                    finished = 1;
                    break;
                }
                agent_state_save(mission, history_buf, hist_len);
                host_submit_patch(pfile);
                vfs_delete(AGENT_STATE_FILE);
                finished = 1;
                break;
            }
        }

        if (find_substr(act, "tool") || (cmd_f[0] != '\0' && !find_substr(act, "final"))) {
            const char *clean_cmd = cmd_f;
            while (*clean_cmd == ' ') clean_cmd++;

            kprint(">> [ACCION IA]: Ejecutando '");
            kprint(clean_cmd);
            kprint("' en el hardware...\n\n");

            tool_feedback_buf[0] = '\0';
            /* AGENT_POLICY_V1: toda herramienta del LLM cruza la frontera segura. */
            dispatch_agent_command(clean_cmd, tool_feedback_buf,
                                   sizeof(tool_feedback_buf));

            kprint("\n>> [FEEDBACK A LA IA]: ");
            kprint(tool_feedback_buf);
            kprint("\n");

            const char *a1 = "- Paso "; while (*a1 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a1++;
            history_buf[hist_len++] = (char)('0' + step);
            const char *a2 = ": ejecutaste '"; while (*a2 && hist_len < sizeof(history_buf) - 200) history_buf[hist_len++] = *a2++;
            const char *a3 = clean_cmd; while (*a3 && hist_len < sizeof(history_buf) - 150) history_buf[hist_len++] = *a3++;
            const char *a4 = "' -> Resultado: '"; while (*a4 && hist_len < sizeof(history_buf) - 100) history_buf[hist_len++] = *a4++;
            const char *a5 = tool_feedback_buf; while (*a5 && hist_len < sizeof(history_buf) - 20) history_buf[hist_len++] = *a5++;
            const char *a6 = "'\n"; while (*a6 && hist_len < sizeof(history_buf) - 1) history_buf[hist_len++] = *a6++;
            history_buf[hist_len] = '\0';
            continue;
        }

        if (find_substr(act, "final") || verdict[0] != '\0') {
            int is_creator_query = (find_substr(mission, "creador") != 0);
            int req_file = !is_creator_query && (find_substr(mission, "crea un") != 0 ||
                                                 find_substr(mission, "crear") != 0 ||
                                                 find_substr(mission, "guarda") != 0 ||
                                                 find_substr(mission, "escribe") != 0 ||
                                                 find_substr(mission, "archivo") != 0 ||
                                                 find_substr(mission, ".txt") != 0 ||
                                                 find_substr(mission, ".log") != 0);

            int file_saved = (find_substr(history_buf, "ejecutaste 'write ") != 0 ||
                              find_substr(history_buf, "ejecutaste 'echo ") != 0 ||
                              find_substr(history_buf, " > ") != 0);

            if (req_file && !file_saved && step < 7) {
                kprint("\n>> [GUARDIA SOMA]: Bloqueada alucinacion de guardado mental.\n");
                history_add_note(&hist_len, step, "RECHAZADO: No ejecutaste 'write'. Persiste en disco primero.");
                continue;
            }

            kprint("\n[AGENTE DICTAMEN FINAL]:\n");
            kprint(verdict[0] ? verdict : agent_reply_buf);
            kprint("\n\n");
            finished = 1;
            break;
        }

        history_add_note(&hist_len, step, "respuesta rechazada por formato.");
    }

    if (!finished) kprint("\nMision concluida.\n\n");
}

void hpatch_command(const char *arg)
{
    while (*arg == ' ') arg++;

    if (arg[0] == 'o' && arg[1] == 'k') {
        host_submit_patch("FILE: src/console.h\n<<<<<<< SEARCH\n#define MYOS_CONSOLE_H\n=======\n#define MYOS_CONSOLE_H\n/* hpatch ok */\n>>>>>>> REPLACE\n");
    } else if (arg[0] == 'b' && arg[1] == 'a' && arg[2] == 'd') {
        host_submit_patch("FILE: src/console.h\n<<<<<<< SEARCH\n#define MYOS_CONSOLE_H\n=======\n#define MYOS_CONSOLE_H\n#error hpatch_bad_test\n>>>>>>> REPLACE\n");
    } else if (arg[0] == 'b' && arg[1] == 'o' && arg[2] == 'o' && arg[3] == 't') {
        host_submit_patch("FILE: src/llm.c\n<<<<<<< SEARCH\nint llm_health(void)\n{\n=======\nint llm_health(void)\n{\n    return 0;\n>>>>>>> REPLACE\n");
    } else if (arg[0] == 'r' && arg[1] == 'a' && arg[2] == 'w') {
        arg += 3;
        while (*arg == ' ') arg++;
        uint32_t n = 0;
        while (*arg && n < sizeof(hpatch_buf) - 1) {
            if (arg[0] == '\\' && arg[1] == 'n') {
                hpatch_buf[n++] = '\n';
                arg += 2;
            } else {
                hpatch_buf[n++] = *arg++;
            }
        }
        hpatch_buf[n] = '\0';
        host_submit_patch(hpatch_buf);
    } else {
        kprint("Uso: hpatch ok | hpatch bad | hpatch bootfail | hpatch raw <texto con \\n>\n");
    }
}
