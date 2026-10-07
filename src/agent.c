#include "arch.h"
#include "agent.h"
#include "rtc.h"
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
#define AGENT_HIST_KEEP     4000
#define AGENT_HIST_MARK     "\n@@HISTORY@@\n"

#define MAILBOX_SIZE        8192
#define MAILBOX_LBA         1
#define MAILBOX_HDR         12

#define MAX_STM_ENTRIES     8
#define STM_PROMPT_WINDOW   2
#define MAX_DELEGATION_DEPTH 3

struct agent_profile {
    const char *name;
    const char *display_title;
    const char *role_desc;
    const char *profile_file;
    const char *stm_file;
};

static const struct agent_profile AGENT_REGISTRY[] = {
    {
        "soma",
        "SOMA CORE - AI KERNEL AGENT",
        "Eres SOMA, el agente nativo e inteligencia central de control del sistema operativo. Tu foco es el diagnostico de hardware, administracion de archivos, mantenimiento del kernel y ejecucion segura.",
        "/agent/soma.txt",
        "/agent/soma_stm.txt"
    },
    {
        "buscador",
        "BUSCADOR - WEB & SYSTEM RESEARCH AGENT",
        "Eres Buscador, el agente de exploracion web, investigacion de actualidad, analisis de datos y navegacion del arbol de memoria estructurada de SOMA.",
        "/agent/buscador.txt",
        "/agent/buscador_stm.txt"
    }
};

static const int NUM_AGENTS = sizeof(AGENT_REGISTRY) / sizeof(AGENT_REGISTRY[0]);

static char agent_state_buf[4096];
static char resume_history_stash[4096];
char agent_resume_mission[512];
char agent_resume_name[32];
int  agent_resume_pending = 0;
static int agent_patch_attempts = 0;
static int delegation_depth = 0;

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

static int str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == *b);
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

static void fb_put_dec(char *out, uint32_t max, uint32_t *pos, uint64_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) { fb_putc(out, max, pos, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) fb_putc(out, max, pos, tmp[--n]);
}

static const struct agent_profile *resolve_agent(const char *name)
{
    if (name) {
        for (int i = 0; i < NUM_AGENTS; ++i) {
            if (str_eq_ci(name, AGENT_REGISTRY[i].name)) {
                return &AGENT_REGISTRY[i];
            }
        }
    }
    return &AGENT_REGISTRY[0];
}

/* ====================================================================
 * MEMORIA A CORTO PLAZO (STM) CON VENTANA DESLIZANTE
 * ==================================================================== */

static void stm_load_into_prompt(const struct agent_profile *agent, char *dst, uint32_t max, uint32_t *pos)
{
    static char stm_raw[2048];
    int r = vfs_read(agent->stm_file, stm_raw, sizeof(stm_raw) - 1);
    if (r > 0) {
        stm_raw[r] = '\0';
        fb_puts(dst, max, pos, "\n[MEMORIA RECIENTE (ULTIMAS INTERACCIONES CON EL USUARIO)]:\n");
        
        const char *p = stm_raw;
        int entries = 0;
        while (*p && entries < STM_PROMPT_WINDOW && *pos < max - 300) {
            dst[(*pos)++] = *p++;
            if (*p == '*' && *(p - 1) == '\n') entries++;
        }
        dst[*pos] = '\0';
        fb_puts(dst, max, pos, "\n");
    }
}

static void stm_append_interaction(const struct agent_profile *agent, const char *user_msg, const char *verdict_msg)
{
    static char existing[2048];
    static char updated[3072];
    int r = vfs_read(agent->stm_file, existing, sizeof(existing) - 1);
    if (r < 0) r = 0;
    existing[r] = '\0';

    uint32_t pos = 0;
    fb_puts(updated, sizeof(updated), &pos, "* Usuario: \"");
    const char *um = user_msg;
    while (*um && pos < sizeof(updated) - 500) {
        if (*um != '\n' && *um != '\r') fb_putc(updated, sizeof(updated), &pos, *um);
        um++;
    }
    fb_puts(updated, sizeof(updated), &pos, "\"\n  Respuesta: \"");
    const char *vm = verdict_msg;
    while (*vm && pos < sizeof(updated) - 300) {
        if (*vm != '\n' && *vm != '\r') fb_putc(updated, sizeof(updated), &pos, *vm);
        vm++;
    }
    fb_puts(updated, sizeof(updated), &pos, "\"\n");

    if (r > 0) {
        const char *p = existing;
        int entries = 1;
        while (*p && entries < MAX_STM_ENTRIES && pos < sizeof(updated) - 2) {
            updated[pos++] = *p++;
            if (*p == '*' && *(p - 1) == '\n') entries++;
        }
        updated[pos] = '\0';
    }

    vfs_write(agent->stm_file, updated, pos);
}

/* ====================================================================
 * MEMORIA DEL SISTEMA Y PIZARRA COMPARTIDA
 * ==================================================================== */

static void inject_agent_memory(const struct agent_profile *agent, char *dst, uint32_t max, uint32_t *pos)
{
    fb_puts(dst, max, pos, "\n[MEMORIA DEL SISTEMA]:\n");

    /* 1. Core Profile: Usuario y Entorno Hardware */
    char buf[384];
    int n = vfs_read("/etc/mem_user.txt", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fb_puts(dst, max, pos, "- Usuario: ");
        fb_puts(dst, max, pos, buf);
        fb_puts(dst, max, pos, "\n");
    }
    n = vfs_read("/etc/mem_hw.txt", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fb_puts(dst, max, pos, "- Hardware: ");
        fb_puts(dst, max, pos, buf);
        fb_puts(dst, max, pos, "\n");
    }

    /* 2. Perfil Individual del Agente */
    n = vfs_read(agent->profile_file, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fb_puts(dst, max, pos, "- Perfil Agente: ");
        fb_puts(dst, max, pos, buf);
        fb_puts(dst, max, pos, "\n");
    }

    /* 3. Pizarra Compartida Multi-Agente (Shared Blackboard) */
    n = vfs_read("/agent/blackboard.txt", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fb_puts(dst, max, pos, "- Pizarra Compartida Multi-Agente: ");
        fb_puts(dst, max, pos, buf);
        fb_puts(dst, max, pos, "\n");
    }

    /* 4. Directiva de Memoria Semántica y Delegación */
    fb_puts(dst, max, pos, "- Herramientas Clave: Usa 'mem_search <keyword>' para explorar /mem/ y 'delegate <agente> \"<mision>\"' para coordinar tareas especializadas.\n");

    /* 5. Inyección de Ventana Deslizante de STM */
    stm_load_into_prompt(agent, dst, max, pos);
}

/* ====================================================================
 * DELEGACIÓN JERÁRQUICA MULTI-AGENTE
 * ==================================================================== */

int delegate_cmd(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (!*args) {
        kprint("Uso: delegate <agente> \"<sub-mision>\"\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Uso: delegate <agente> \"<sub-mision>\"");
        return 1;
    }

    char target_agent[32];
    uint32_t ap = 0;
    while (*args && *args != ' ' && ap < sizeof(target_agent) - 1) {
        target_agent[ap++] = *args++;
    }
    target_agent[ap] = '\0';
    while (*args == ' ') args++;

    char q = 0;
    if (*args == '"' || *args == '\'') q = *args++;

    char sub_mission[512];
    uint32_t sp = 0;
    while (*args && sp < sizeof(sub_mission) - 1) {
        if (q && *args == q && *(args + 1) == '\0') break;
        sub_mission[sp++] = *args++;
    }
    sub_mission[sp] = '\0';

    if (sp == 0) {
        kprint("delegate: falta la sub-mision para el agente '"); kprint(target_agent); kprint("'\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Error: sub-mision vacia");
        return 1;
    }

    if (delegation_depth >= MAX_DELEGATION_DEPTH) {
        kprint("delegate: profundidad maxima de delegacion alcanzada (3)\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Error: limite de delegacion alcanzado");
        return 1;
    }

    kprint("\n\033[1;33m[ORQUESTACION MULTI-AGENTE] Delegando tarea a '");
    kprint(target_agent);
    kprint("'...\033[0m\n");

    delegation_depth++;
    agent_run(target_agent, sub_mission);
    delegation_depth--;

    static char bb[1024];
    int r = vfs_read("/agent/blackboard.txt", bb, sizeof(bb) - 1);
    if (r > 0) bb[r] = '\0';
    else bb[0] = '\0';

    uint32_t pos = 0;
    fb_puts(out_buf, max_out, &pos, "[DELEGACION A ");
    fb_puts(out_buf, max_out, &pos, target_agent);
    fb_puts(out_buf, max_out, &pos, " COMPLETADA]: ");
    if (bb[0]) {
        fb_puts(out_buf, max_out, &pos, bb);
    } else {
        fb_puts(out_buf, max_out, &pos, "Sub-mision ejecutada exitosamente.");
    }

    return 1;
}

/* ====================================================================
 * COMANDOS DE MEMORIA ESTRUCTURADA EN ÁRBOL Y BÚSQUEDA
 * ==================================================================== */

int mem_search_cmd(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (!*args) {
        kprint("Uso: mem_search <palabras_clave>\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Uso: mem_search <palabras_clave>");
        return 1;
    }
    static char search_res[2048];
    search_res[0] = '\0';
    int hits = vfs_search_bm25(args, "", search_res, sizeof(search_res));

    uint32_t pos = 0;
    if (hits <= 0) {
        kprint("mem_search (BM25): sin coincidencias para '"); kprint(args); kprint("'\n");
        fb_puts(out_buf, max_out, &pos, "Sin coincidencias en memoria persistente.");
    } else {
        kprint("\n--- Resultados BM25 en Memoria Persistente ---\n");
        kprint(search_res);
        kprint("----------------------------------------------\n");
        fb_puts(out_buf, max_out, &pos, search_res);
    }
    return 1;
}

int mem_tree_cmd(const char *args, char *out_buf, uint32_t max_out)
{
    (void)args;
    static char tree_idx[1024];
    uint32_t pos = 0;
    int r = vfs_read("/mem/indice.txt", tree_idx, sizeof(tree_idx) - 1);
    if (r > 0) {
        tree_idx[r] = '\0';
        kprint("\nARBOL DE MEMORIA ESTRUCTURADA:\n--------------------------------\n");
        kprint(tree_idx);
        kprint("--------------------------------\n");
        fb_puts(out_buf, max_out, &pos, tree_idx);
    } else {
        const char *fallback = "/mem/ (Arbol de Memoria)\n+-- ciencia/\n|   +-- ciencia_fisica.txt\n|   +-- ciencia_computacion.txt\n+-- historia/\n|   +-- historia_antigua.txt\n|   +-- historia_moderna.txt\n+-- conversaciones/\n    +-- conversaciones.txt\n";
        kprint(fallback);
        fb_puts(out_buf, max_out, &pos, fallback);
    }
    return 1;
}

int mem_read_cmd(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    if (!*args) {
        kprint("Uso: mem_read <nodo_o_ruta>  (ejemplo: mem_read /mem/historia_moderna.txt)\n");
        uint32_t p = 0; fb_puts(out_buf, max_out, &p, "Uso: mem_read <nodo>");
        return 1;
    }

    char target[64];
    uint32_t tp = 0;
    if (args[0] != '/') {
        fb_puts(target, sizeof(target), &tp, "/mem/");
    }
    while (*args && *args != ' ' && tp < sizeof(target) - 1) {
        target[tp++] = *args++;
    }
    target[tp] = '\0';

    static char node_data[2048];
    int r = vfs_read(target, node_data, sizeof(node_data) - 1);
    uint32_t pos = 0;
    if (r >= 0) {
        node_data[r] = '\0';
        kprint("\n[NODO "); kprint(target); kprint("]:\n");
        kprint(node_data);
        kprint("\n");
        fb_puts(out_buf, max_out, &pos, node_data);
    } else {
        kprint("mem_read: nodo no encontrado '"); kprint(target); kprint("'\n");
        fb_puts(out_buf, max_out, &pos, "Error: nodo de memoria no encontrado.");
    }
    return 1;
}

int mem_write_cmd(const char *args, char *out_buf, uint32_t max_out)
{
    while (*args == ' ') args++;
    char node[64];
    uint32_t np = 0;
    if (*args != '/') fb_puts(node, sizeof(node), &np, "/mem/");
    while (*args && *args != ' ' && np < sizeof(node) - 1) node[np++] = *args++;
    node[np] = '\0';
    while (*args == ' ') args++;

    char q = 0;
    if (*args == '"' || *args == '\'') q = *args++;

    char text[2048];
    uint32_t tp = 0;
    while (*args && tp < sizeof(text) - 1) {
        if (q && *args == q && *(args + 1) == '\0') break;
        text[tp++] = *args++;
    }
    text[tp] = '\0';

    int r = vfs_write(node, text, tp);
    uint32_t pos = 0;
    if (r >= 0) {
        kprint("mem_write: nodo '"); kprint(node); kprint("' actualizado (");
        kprint_dec(tp); kprint(" bytes)\n");
        fb_puts(out_buf, max_out, &pos, "Nodo de memoria actualizado.");
    } else {
        kprint("mem_write: error al escribir nodo '"); kprint(node); kprint("'\n");
        fb_puts(out_buf, max_out, &pos, "Error al escribir nodo de memoria.");
    }
    return 1;
}

/* ====================================================================
 * GESTIÓN DE PARCHES Y ESTADO DE REANUDACIÓN
 * ==================================================================== */

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

static void agent_state_save(const char *agent_name, const char *mission, const char *history, uint32_t hist_len)
{
    static uint8_t zero_sec[512];
    uint32_t pos = 0;
    uint32_t start = hist_len > AGENT_HIST_KEEP ? hist_len - AGENT_HIST_KEEP : 0;

    if (start > 0) {
        const char *nx = find_substr(history + start, "\n- Paso ");
        start = nx ? (uint32_t)(nx + 1 - history) : hist_len;
    }

    agent_state_buf[0] = '\0';
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "AGENT:");
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, agent_name);
    fb_puts(agent_state_buf, sizeof(agent_state_buf), &pos, "\nATTEMPTS:");
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
    agent_resume_name[0] = '\0';
    if (memcmp(p, "AGENT:", 6) == 0) {
        p += 6;
        uint32_t ap = 0;
        while (*p && *p != '\n' && ap < sizeof(agent_resume_name) - 1) {
            agent_resume_name[ap++] = *p++;
        }
        agent_resume_name[ap] = '\0';
        if (*p == '\n') p++;
    }
    if (agent_resume_name[0] == '\0') {
        fb_puts(agent_resume_name, sizeof(agent_resume_name), &(uint32_t){0}, "soma");
    }

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
    resume_history_stash[0] = '\0';
    fb_puts(resume_history_stash, sizeof(resume_history_stash), &pos, hm + (sizeof(AGENT_HIST_MARK) - 1));
    if (pos > 0 && resume_history_stash[pos - 1] != '\n') fb_putc(resume_history_stash, sizeof(resume_history_stash), &pos, '\n');

    if (virtio_blk_read(RESULT_LBA, res) == 0 && res[0] != 0) {
        res[511] = 0;
        for (int i = 0; res[i]; ++i) if (res[i] < 32 || res[i] > 126) res[i] = ' ';
    } else {
        res[0] = '\0';
    }

    if (find_substr((const char *)res, "BOOT_OK")) {
        fb_puts(resume_history_stash, sizeof(resume_history_stash), &pos,
                "- Paso R: El host APROBO y COMPILO tu parche con exito (BOOT_OK).\n"
                "  [DIRECTIVA]: Tu parche ya esta aplicado. NO envies mas parches. Concluye con action=\"final\".\n");
    } else if (find_substr((const char *)res, "FAILED") || find_substr((const char *)res, "REJECTED")) {
        fb_puts(resume_history_stash, sizeof(resume_history_stash), &pos,
                "- Paso R: El parche NO fue aceptado (rollback aplicado).\n  [DETALLE]: ");
        fb_puts(resume_history_stash, sizeof(resume_history_stash), &pos, (const char *)res);
        fb_puts(resume_history_stash, sizeof(resume_history_stash), &pos, "\n  [DIRECTIVA]: Corrige tu SEARCH/REPLACE y reintenta.\n");
    }

    agent_patch_attempts = n;
    agent_resume_pending = 1;
}

static void history_add_note(char *hbuf, uint32_t *hist_len, uint32_t max_hist, int step, const char *msg)
{
    if (!hbuf || !hist_len || *hist_len >= max_hist - 400) return;
    uint32_t pos = *hist_len;
    fb_puts(hbuf, max_hist, &pos, "- Paso ");
    fb_putc(hbuf, max_hist, &pos, (char)('0' + step));
    fb_puts(hbuf, max_hist, &pos, ": ");
    fb_puts(hbuf, max_hist, &pos, msg);
    fb_puts(hbuf, max_hist, &pos, "\n");
    *hist_len = pos;
}



/* ====================================================================
 * BUCLE AUTÓNOMO REACT MULTI-AGENTE (Contexto Aislado en KHeap)
 * ==================================================================== */

void agent_run(const char *agent_name, const char *mission)
{
    const struct agent_profile *agent = resolve_agent(agent_name);

    while (*mission == ' ') mission++;
    if (*mission == '\0') {
        kprint("Uso: "); kprint(agent->name); kprint(" <mision en lenguaje natural>\n");
        return;
    }

    /* Asignación dinámica por invocación para soportar delegación recursiva sin colisión */
    char *agent_prompt_buf  = (char *)kmalloc(16384);
    char *agent_reply_buf   = (char *)kmalloc(4096);
    char *tool_feedback_buf = (char *)kmalloc(4096);
    char *history_buf       = (char *)kmalloc(8192);

    if (!agent_prompt_buf || !agent_reply_buf || !tool_feedback_buf || !history_buf) {
        kprint("agent: error de memoria al inicializar contexto de agente.\n");
        if (agent_prompt_buf) kfree(agent_prompt_buf);
        if (agent_reply_buf) kfree(agent_reply_buf);
        if (tool_feedback_buf) kfree(tool_feedback_buf);
        if (history_buf) kfree(history_buf);
        return;
    }

    kprint("\n\033[1;35m===============================================================\033[0m\n");
    kprint(" \033[1;36m");
    kprint(agent->display_title);
    kprint("\033[0m\n");
    kprint("\033[1;35m===============================================================\033[0m\n");
    kprint(" Mision: \033[1;37m");
    kprint(mission);
    kprint("\033[0m\n");

    uint32_t hist_len = 0;
    if (agent_resume_pending == 1) {
        agent_resume_pending = 0;
        fb_puts(history_buf, 8192, &hist_len, resume_history_stash);
    } else {
        history_buf[0] = '\0';
    }

    uint32_t ap_len = 0;
    int finished = 0;

    for (int step = 1; step <= 6; ++step) {
        ap_len = 0;
        struct rtc_time cur_t;
        rtc_get_datetime(&cur_t);
        uint32_t cur_y = cur_t.year, cur_mo = cur_t.month, cur_d = cur_t.day;
        uint32_t cur_h = cur_t.hour + 2;
        if (cur_h >= 24) { cur_h -= 24; cur_d += 1; }

        fb_puts(agent_prompt_buf, 16384, &ap_len, "IDENTIDAD: ");
        fb_puts(agent_prompt_buf, 16384, &ap_len, agent->role_desc);
        fb_puts(agent_prompt_buf, 16384, &ap_len, "\n\nMISION DEL USUARIO: '");
        const char *p_mval = mission;
        while (*p_mval && ap_len < 16384 - 2400) agent_prompt_buf[ap_len++] = *p_mval++;

        fb_puts(agent_prompt_buf, 16384, &ap_len, "'.\n"
                            "Entorno: SOMA OS Bare-Metal (" MYOS_ARCH_NAME ") | IP 10.0.2.15 | RamFS en /.\n"
                            "FECHA ACTUAL DEL SISTEMA: ");
        fb_put_dec(agent_prompt_buf, 16384, &ap_len, cur_y);
        fb_putc(agent_prompt_buf, 16384, &ap_len, '-');
        if (cur_mo < 10) fb_putc(agent_prompt_buf, 16384, &ap_len, '0');
        fb_put_dec(agent_prompt_buf, 16384, &ap_len, cur_mo);
        fb_putc(agent_prompt_buf, 16384, &ap_len, '-');
        if (cur_d < 10) fb_putc(agent_prompt_buf, 16384, &ap_len, '0');
        fb_put_dec(agent_prompt_buf, 16384, &ap_len, cur_d);
        fb_puts(agent_prompt_buf, 16384, &ap_len, " (CEST)\n\n");

        const char *p_ctx = "HERRAMIENTAS BARE-METAL DISPONIBLES:\n"
                            "  delegate <agente> \"<mision>\"  Delega una sub-mision especializada a otro agente ('buscador' o 'soma').\n"
                            "  search <consulta>         Busca informacion web en vivo (noticias, eventos, fechas, datos).\n"
                            "  mem_search <termino>      Busca conceptos o palabras clave en el arbol de memoria /mem/.\n"
                            "  mem_tree                  Muestra el arbol general de memoria estructurada.\n"
                            "  mem_read <nodo>           Lee el contenido de una rama/nodo del arbol de memoria.\n"
                            "  mem_write <nodo> \"<txt>\"  Actualiza o crea un nodo en el arbol de memoria.\n"
                            "  ls [-a] [ruta]            Lista los archivos del sistema RamFS (oculta archivos de sistema por defecto).\n"
                            "  cat <ruta>                Lee el contenido de un archivo.\n"
                            "  write <ruta> \"<texto>\"    Crea o sobrescribe un archivo con texto.\n"
                            "  touch <ruta> | rm <ruta>  Crea archivo vacio o elimina archivo.\n"
                            "  cp <src> <dst> | mv <src> <dst>\n"
                            "  grep [-i] [-l] <patron> [arch]\n"
                            "  free | df | status | uptime | date | env\n"
                            "  ping <ip> | dns <host> | curl <host> [puerto] [ruta]\n"
                            "  src_ls | src_cat <f> [off] | src_grep <f> <pat>\n"
                            "\n"
                            "FORMATO OBLIGATORIO DE RESPUESTA JSON:\n"
                            "{\"thought\":\"analisis paso a paso\",\"action\":\"tool|patch|final\","
                            "\"cmd\":\"comando\",\"patch\":{\"file\":\"src/f.c\","
                            "\"search\":\"exacto\",\"replace\":\"nuevo\"},"
                            "\"verdict\":\"respuesta final si action=final\"}\n"
                            "\n"
                            "POLITICA DE DIRECTORIOS Y ESPACIOS DE TRABAJO:\n"
                            "1) RUTA PREFERENTE PARA AGENTES: Si creas notas, informes o borradores generados por la IA, usa preferentemente '/workspace/' (ej. '/workspace/notas.txt' o '/workspace/informe.txt') salvo que el usuario pida otra ruta explicita.\n"
                            "2) ARCHIVOS DE USUARIO: Los documentos y notas personales del usuario se encuentran en '/mis_archivos/'.\n"
                            "3) Si la mision pide crear/guardar/modificar un archivo tras buscar o procesar, DEBES ejecutar 'write <ruta> \"<contenido>\"' en un paso intermedio ANTES de emitir action='final'.\n"
                            "4) Solo emite action='final' cuando TODOS los objetivos de la mision esten completamente cumplidos en el hardware.\n";
        fb_puts(agent_prompt_buf, 16384, &ap_len, p_ctx);

        inject_agent_memory(agent, agent_prompt_buf, 16384, &ap_len);

        if (hist_len > 0) {
            const char *h_hdr = "\nAcciones ya ejecutadas en pasos anteriores:\n";
            while (*h_hdr) agent_prompt_buf[ap_len++] = *h_hdr++;
            for (uint32_t h = 0; h < hist_len && ap_len < 16384 - 600; ++h) {
                agent_prompt_buf[ap_len++] = history_buf[h];
            }
        }

        const char *p_rules = "\nInstruccion: Razona que falta para cumplir la mision y genera el objeto JSON.";
        while (*p_rules && ap_len < 16384 - 1) agent_prompt_buf[ap_len++] = *p_rules++;
        agent_prompt_buf[ap_len] = '\0';

        if (!llm_chat_json(agent_prompt_buf, agent_reply_buf, 4096, 40000)) {
            kprint("Error en comunicacion estructurada con el agente.\n");
            break;
        }

        static char act[32], th[4096], cmd_f[16384], patch_f[4096], verdict[4096];
        act[0] = '\0'; th[0] = '\0'; cmd_f[0] = '\0'; patch_f[0] = '\0'; verdict[0] = '\0';

        llm_json_get(agent_reply_buf, "action", act, sizeof(act));
        llm_json_get(agent_reply_buf, "thought", th, sizeof(th));
        llm_json_get(agent_reply_buf, "cmd", cmd_f, sizeof(cmd_f));
        llm_json_get(agent_reply_buf, "patch", patch_f, sizeof(patch_f));
        llm_json_get(agent_reply_buf, "verdict", verdict, sizeof(verdict));

        if (th[0]) {
            kprint("\n  \033[1;35m+-------------------------------------------------------------+\033[0m\n");
            kprint("  \033[1;35m| [Paso ");
            kprint_dec((uint32_t)step);
            kprint(" : Razonamiento IA (");
            kprint(agent->name);
            kprint(")]\033[0m\n");
            kprint("  \033[1;35m+-------------------------------------------------------------+\033[0m\n");
            kprint("   ");
            kprint(th);
            kprint("\n");
        }

        /* Gestión de Parches de Kernel */
                if (find_substr(act, "patch")) {
            const char *ptext = patch_f[0] ? patch_f : agent_reply_buf;
            const char *pfile = find_substr(ptext, "FILE: src/");
            if (pfile) {
                const char *tf = pfile + 10;
                while (*tf == ' ') tf++;
                int is_c_src = (find_substr(tf, ".c") != 0 || find_substr(tf, ".h") != 0 || find_substr(tf, ".s") != 0);
                int is_workspace = (find_substr(tf, "workspace") != 0 || find_substr(tf, "mis_archivos") != 0 || find_substr(tf, ".txt") != 0);

                if (is_c_src && !is_workspace) {
                    if (is_patch_target_protected(tf)) {
                        kprint("\n[SEGURIDAD KERNEL] Modificacion denegada: Subsistema protegido.\n");
                        history_add_note(history_buf, &hist_len, 8192, step, "tu parche fue rechazado: los subsistemas del core estan PROTEGIDOS.");
                        continue;
                    }
                    kprint("\n[AGENTE AUTONOMO]: Parche de kernel C emitido. Enviando al host...\n");
                    if (agent_patch_attempts >= AGENT_MAX_PATCHES) {
                        kprint("\n[AGENTE AUTONOMO]: limite de parches por mision alcanzado.\n");
                        finished = 1;
                        break;
                    }
                    agent_state_save(agent->name, mission, history_buf, hist_len);
                    host_submit_patch(pfile);
                    vfs_delete(AGENT_STATE_FILE);
                    finished = 1;
                    break;
                }
            }
            kprint("\n[AVISO KERNEL]: 'patch' interceptado. Los archivos en RamFS (/workspace/) deben crearse con 'write'.\n");
            history_add_note(history_buf, &hist_len, 8192, step, "RECHAZADO: 'patch' es exclusivo para codigo del kernel (src/*.c). Para crear o editar archivos en RamFS o /workspace/ DEBES usar action='tool' con 'write <ruta> \"<contenido>\"'.");
            continue;
        }

        if (find_substr(act, "tool") || (cmd_f[0] != '\0' && !find_substr(act, "final"))) {
            const char *clean_cmd = cmd_f;
            while (*clean_cmd == ' ') clean_cmd++;

            kprint("\n  \033[1;33m[Accion Hardware] -> Ejecutando:\033[0m \033[1;32m$ ");
            kprint(clean_cmd);
            kprint("\033[0m\n");

            tool_feedback_buf[0] = '\0';
            dispatch_agent_command(clean_cmd, tool_feedback_buf, 4096);

            kprint("  \033[0;36m+--- [Resultado Hardware] ----------------------------------+\033[0m\n");
            kprint("  \033[0;36m| \033[0m");
            kprint(tool_feedback_buf);
            kprint("\n  \033[0;36m+-----------------------------------------------------------+\033[0m\n");

            uint32_t fb_len = 0;
            while (tool_feedback_buf[fb_len]) fb_len++;
            vfs_write("/tmp/last_tool.log", tool_feedback_buf, fb_len);

            fb_puts(history_buf, 8192, &hist_len, "- Paso ");
            fb_putc(history_buf, 8192, &hist_len, (char)('0' + step));
            fb_puts(history_buf, 8192, &hist_len, ": ejecutaste '");
            fb_puts(history_buf, 8192, &hist_len, clean_cmd);
            fb_puts(history_buf, 8192, &hist_len, "' -> Resultado: '");

            if (fb_len > 450) {
                for (uint32_t k = 0; k < 400 && hist_len < 8192 - 200; ++k) {
                    history_buf[hist_len++] = tool_feedback_buf[k];
                }
                fb_puts(history_buf, 8192, &hist_len, " [... salida resumida; completa en /tmp/last_tool.log ...]");
            } else {
                for (uint32_t k = 0; k < fb_len && hist_len < 8192 - 30; ++k) {
                    history_buf[hist_len++] = tool_feedback_buf[k];
                }
            }
            fb_puts(history_buf, 8192, &hist_len, "'\n");
            history_buf[hist_len] = '\0';
            continue;
        }

        /* Evaluación de Dictamen Final */
        if (find_substr(act, "final") || verdict[0] != '\0') {
            /* Manejador Dual: si emitió final pero incluyó un comando write en el mismo paso, ejecutarlo */
            if (cmd_f[0] != '\0') {
                const char *clean_cmd = cmd_f;
                while (*clean_cmd == ' ') clean_cmd++;
                kprint("\n  \033[1;33m[Accion Hardware Pre-Final] -> Ejecutando:\033[0m \033[1;32m$ ");
                kprint(clean_cmd);
                kprint("\033[0m\n");
                tool_feedback_buf[0] = '\0';
                dispatch_agent_command(clean_cmd, tool_feedback_buf, 4096);
            }

            int demands_file = (find_substr(mission, "crea") != 0 ||
                                find_substr(mission, "crear") != 0 ||
                                find_substr(mission, "guarda") != 0 ||
                                find_substr(mission, "guardar") != 0 ||
                                find_substr(mission, "escribe") != 0 ||
                                find_substr(mission, "escribir") != 0 ||
                                find_substr(mission, "copiar su contenido") != 0 ||
                                find_substr(mission, "copia su contenido") != 0 ||
                                find_substr(mission, ".txt") != 0 ||
                                find_substr(mission, ".sh") != 0 ||
                                find_substr(mission, ".log") != 0);

            int file_written = (find_substr(history_buf, "ejecutaste 'write ") != 0 ||
                                find_substr(history_buf, "ejecutaste 'echo ") != 0 ||
                                find_substr(history_buf, "ejecutaste 'touch ") != 0 ||
                                find_substr(history_buf, " > ") != 0 ||
                                (cmd_f[0] == 'w' && cmd_f[1] == 'r' && cmd_f[2] == 'i'));

            if (demands_file && !file_written && step < 6) {
                kprint("\n>> [GUARDIA SOMA]: Bloqueada alucinacion. Falta escribir fisicamente el archivo.\n");
                history_add_note(history_buf, &hist_len, 8192, step, "RECHAZADO: La mision pide crear/guardar un archivo. Debes ejecutar 'write <ruta> \"<contenido>\"' antes de finalizar.");
                continue;
            }

            const char *v_final = verdict[0] ? verdict : (th[0] ? th : "Mision completada y cambios persistidos en el sistema.");
            kprint("\n\033[1;32m===============================================================\033[0m\n");
            kprint(" \033[1;32m[");
            kprint(agent->name);
            kprint("] - DICTAMEN FINAL\033[0m\n");
            kprint("\033[1;32m===============================================================\033[0m\n");
            kprint("  \033[1;36m");
            kprint(v_final);
            kprint("\033[0m\n\033[1;32m===============================================================\033[0m\n\n");

            stm_append_interaction(agent, mission, v_final);

            char bb_update[512];
            uint32_t bbp = 0;
            fb_puts(bb_update, sizeof(bb_update), &bbp, "Ultimo dictamen de [");
            fb_puts(bb_update, sizeof(bb_update), &bbp, agent->name);
            fb_puts(bb_update, sizeof(bb_update), &bbp, "]: ");
            const char *vp = v_final;
            while (*vp && bbp < sizeof(bb_update) - 10) {
                if (*vp != '\n' && *vp != '\r') bb_update[bbp++] = *vp;
                vp++;
            }
            bb_update[bbp] = '\0';
            vfs_write("/agent/blackboard.txt", bb_update, bbp);

            finished = 1;
            break;
        }

        history_add_note(history_buf, &hist_len, 8192, step, "formato JSON no reconocido.");
    }

    if (!finished) {
        kprint("\n>> [SOMA AGENT]: Sintetizando dictamen con la informacion disponible...\n");
        ap_len = 0;
        fb_puts(agent_prompt_buf, 16384, &ap_len, "MISION: '");
        const char *pm = mission;
        while (*pm && ap_len < 16384 - 2200) agent_prompt_buf[ap_len++] = *pm++;
        fb_puts(agent_prompt_buf, 16384, &ap_len, "'\n\nHistorial de acciones previas:\n");
        for (uint32_t h = 0; h < hist_len && ap_len < 16384 - 800; ++h) {
            agent_prompt_buf[ap_len++] = history_buf[h];
        }
        fb_puts(agent_prompt_buf, 16384, &ap_len,
                "\nInstruccion: Responde de forma veraz y directa al usuario basandote estrictamente en los hechos ejecutados.");

        if (llm_chat(agent_prompt_buf, agent_reply_buf, 4096, 30000)) {
            kprint("\n\033[1;32m===============================================================\033[0m\n");
            kprint(" \033[1;32m[");
            kprint(agent->name);
            kprint("] - DICTAMEN FINAL\033[0m\n");
            kprint("\033[1;32m===============================================================\033[0m\n");
            kprint("  \033[1;36m");
            kprint(agent_reply_buf);
            kprint("\033[0m\n\033[1;32m===============================================================\033[0m\n\n");
            stm_append_interaction(agent, mission, agent_reply_buf);

            char bb_update[512];
            uint32_t bbp = 0;
            fb_puts(bb_update, sizeof(bb_update), &bbp, "Ultimo dictamen de [");
            fb_puts(bb_update, sizeof(bb_update), &bbp, agent->name);
            fb_puts(bb_update, sizeof(bb_update), &bbp, "]: ");
            const char *vp = agent_reply_buf;
            while (*vp && bbp < sizeof(bb_update) - 10) {
                if (*vp != '\n' && *vp != '\r') bb_update[bbp++] = *vp;
                vp++;
            }
            bb_update[bbp] = '\0';
            vfs_write("/agent/blackboard.txt", bb_update, bbp);
        } else {
            kprint("\nMision finalizada.\n\n");
        }
    }

    kfree(agent_prompt_buf);
    kfree(agent_reply_buf);
    kfree(tool_feedback_buf);
    kfree(history_buf);
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
