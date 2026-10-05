#include <stdint.h>

#include "console.h"
#include "http.h"
#include "llm.h"
#include "mem.h"
#include "net.h"
#include "sysinfo.h"

static const uint8_t llm_ip[4] = { 192, 168, 1, 200 };
static const uint16_t llm_port = 8087;

char llm_last_reply[4096];
int  llm_test_passed = 0;

static char http_resp_buf[32768];
static char payload_buf[16384];

static uint32_t my_strlen(const char *s)
{
    uint32_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

static const char *strstr_simple(const char *haystack, const char *needle)
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
        if (!*n) {
            return haystack;
        }
    }
    return 0;
}

static int build_chat_payload_mode(const char *prompt, char *dst, uint32_t max, int json_mode)
{
    const char *prefix = 
        "{\"model\":\"nail-35b\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"Eres el nucleo de inteligencia artificial de MYOS x86_64. No uses bloques de pensamiento <think>. Responde estrictamente con el objeto JSON solicitado sin preambulos.\"},"
        "{\"role\":\"user\",\"content\":\"";

    const char *suffix_plain = "\"}],\"temperature\":0.2,\"max_tokens\":2048}";
    const char *suffix_json  = "\"}],\"response_format\":{\"type\":\"json_object\",\"schema\":{\"type\":\"object\",\"properties\":{\"thought\":{\"type\":\"string\"},\"action\":{\"type\":\"string\",\"enum\":[\"tool\",\"patch\",\"final\"]},\"cmd\":{\"type\":\"string\"},\"patch\":{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\"},\"search\":{\"type\":\"string\"},\"replace\":{\"type\":\"string\"}},\"required\":[\"file\",\"search\",\"replace\"]},\"verdict\":{\"type\":\"string\"}},\"required\":[\"thought\",\"action\",\"cmd\",\"patch\",\"verdict\"]}},\"chat_template_kwargs\":{\"enable_thinking\":false},\"temperature\":0.1,\"max_tokens\":4096}";
    const char *suffix = json_mode ? suffix_json : suffix_plain;

    uint32_t p = 0;
    while (*prefix && p < max - 1) {
        dst[p++] = *prefix++;
    }

    for (const char *s = prompt; *s && p < max - 1; ++s) {
        if (*s == '"' || *s == '\\') {
            if (p >= max - 2) return -1;
            dst[p++] = '\\';
            dst[p++] = *s;
        } else if (*s == '\n') {
            if (p >= max - 2) return -1;
            dst[p++] = '\\';
            dst[p++] = 'n';
        } else if (*s == '\t') {
            if (p >= max - 2) return -1;
            dst[p++] = '\\';
            dst[p++] = 't';
        } else if (*s == '\r' || (unsigned char)*s < 0x20) {
            /* omitir controles */
        } else {
            dst[p++] = *s;
        }
    }

    while (*suffix && p < max - 1) {
        dst[p++] = *suffix++;
    }

    if (p >= max) return -1;
    dst[p] = '\0';
    return (int)p;
}

static int build_chat_payload(const char *prompt, char *dst, uint32_t max)
{
    return build_chat_payload_mode(prompt, dst, max, 0);
}

static int extract_json_field(const char *json, const char *key, char *out, uint32_t out_max)
{
    if (!json || !key || !out || out_max == 0) {
        return 0;
    }

    const char *p = json;

    for (;;) {
        p = strstr_simple(p, key);
        if (!p) {
            return 0;
        }

        p += my_strlen(key);

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }

        if (*p != ':') {
            continue;
        }

        p++;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }

        if (*p != '"') {
            continue;
        }

        p++;
        break;
    }

    uint32_t len = 0;
    while (*p && *p != '"' && len < out_max - 1) {
        if (*p == '\\') {
            p++;
            if (*p == '"') {
                out[len++] = '"';
            } else if (*p == '\\') {
                out[len++] = '\\';
            } else if (*p == 'n') {
                out[len++] = '\n';
            } else if (*p == 'r') {
                out[len++] = '\r';
            } else if (*p == 't') {
                out[len++] = '\t';
            } else if (*p) {
                out[len++] = *p;
            } else {
                break;
            }
            if (*p) p++;
        } else {
            out[len++] = *p++;
        }
    }

    out[len] = '\0';
    return (int)len;
}

static int extract_json_content(const char *json, char *out, uint32_t out_max)
{
    /* 1. Prioridad: el campo 'content' (respuesta final) */
    int len = extract_json_field(json, "\"content\"", out, out_max);

    /* 2. Si 'content' viniera vacio, fallback a 'reasoning_content' */
    if (len <= 0) {
        len = extract_json_field(json, "\"reasoning_content\"", out, out_max);
    }

    if (len <= 0) {
        return 0;
    }

    /* 3. Si contiene bloques <think>...</think>, limpiar */
    const char *end_think = strstr_simple(out, "</think>");
    if (end_think) {
        end_think += 8;
        while (*end_think == ' ' || *end_think == '\t' ||
               *end_think == '\r' || *end_think == '\n') {
            end_think++;
        }
        if (*end_think != '\0') {
            uint32_t new_len = 0;
            while (end_think[new_len] && new_len < out_max - 1) {
                out[new_len] = end_think[new_len];
                new_len++;
            }
            out[new_len] = '\0';
            len = (int)new_len;
        }
    }

    /* 4. Limpieza de bordes */
    char *start = out;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != out) {
        uint32_t i = 0;
        while (start[i]) {
            out[i] = start[i];
            i++;
        }
        out[i] = '\0';
        len = (int)i;
    }

    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' ||
                       out[len - 1] == '\r' || out[len - 1] == '\n')) {
        out[--len] = '\0';
    }

    return len > 0 ? len : 0;
}

static int llm_health_unlocked(void)
{
    struct http_response resp;
    int code = http_get(llm_ip, llm_port, "/health", http_resp_buf, sizeof(http_resp_buf), &resp, 5000);

    if (code == 200) {
        return 1;
    }
    return 0;
}


int llm_json_get(const char *json, const char *key, char *out, uint32_t out_max)
{
    if (!json || !key || !out || out_max == 0) return 0;
    char quoted_key[64];
    if (key[0] == '"') {
        return extract_json_field(json, key, out, out_max);
    }
    uint32_t klen = 0;
    quoted_key[klen++] = '"';
    while (key[klen - 1] && klen < sizeof(quoted_key) - 2) {
        quoted_key[klen] = key[klen - 1];
        klen++;
    }
    quoted_key[klen++] = '"';
    quoted_key[klen] = '\0';
    return extract_json_field(json, quoted_key, out, out_max);
}

static int llm_chat_json_unlocked(const char *prompt, char *reply_out, uint32_t reply_max, uint32_t timeout_ms)
{
    if (build_chat_payload_mode(prompt, payload_buf, sizeof(payload_buf), 1) < 0) {
        kprint("LLM: payload too large\n");
        return 0;
    }

    struct http_response resp;
    int code = http_post_json(
        llm_ip,
        llm_port,
        "/v1/chat/completions",
        payload_buf,
        http_resp_buf,
        sizeof(http_resp_buf),
        &resp,
        timeout_ms
    );

    if (code != 200 || !resp.body) {
        return 0;
    }

    return extract_json_content(resp.body, reply_out, reply_max);
}
static int llm_chat_unlocked(const char *prompt, char *reply_out, uint32_t reply_max, uint32_t timeout_ms)
{
    if (build_chat_payload(prompt, payload_buf, sizeof(payload_buf)) < 0) {
        kprint("LLM: payload too large\n");
        return 0;
    }

    struct http_response resp;
    int code = http_post_json(
        llm_ip,
        llm_port,
        "/v1/chat/completions",
        payload_buf,
        http_resp_buf,
        sizeof(http_resp_buf),
        &resp,
        timeout_ms
    );

    if (code != 200) {
        kprint("LLM: HTTP error status ");
        if (code >= 0) {
            kprint_dec((uint32_t)code);
        } else {
            kprint("CONN_ERR");
        }
        kprint("\n");
        return 0;
    }

    if (!resp.body) {
        kprint("LLM: empty HTTP response body\n");
        return 0;
    }

    if (!extract_json_content(resp.body, reply_out, reply_max)) {
        kprint("LLM: could not extract content from JSON response\n");
        return 0;
    }

    return 1;
}

void net_run_llm_test(void)
{
    kprint("\n============================================================\n");
    kprint("MYOS - HTTP & LLM INTEGRATION TEST\n");
    kprint("Target server: ");
    kprint_ip(llm_ip);
    kprint(":8087 (model: nail-35b)\n");
    kprint("============================================================\n");

    if (!net_init()) {
        kprint("NET: init FAILED\n");
        return;
    }

    kprint("\n[STEP 1] Testing HTTP GET /health...\n");
    if (!llm_health()) {
        kprint("LLM: health check FAILED\n");
        return;
    }
    kprint("LLM: health check OK (HTTP 200 OK)\n");

    kprint("\n[STEP 2] Testing LLM Chat Completion (POST /v1/chat/completions)...\n");
    static const char test_prompt[] = "Responde unicamente: MYOS TEST OK";
    kprint("Prompt: \"");
    kprint(test_prompt);
    kprint("\"\n");
    kprint("Waiting for nail-35b to generate response...\n");

    memset(llm_last_reply, 0, sizeof(llm_last_reply));

    if (llm_chat(test_prompt, llm_last_reply, sizeof(llm_last_reply), 30000)) {
        kprint("============================================================\n");
        kprint("LLM RESPONSE EXTRACTED:\n");
        kprint(">> ");
        kprint(llm_last_reply);
        kprint("\n============================================================\n");

        llm_test_passed = 1;
        kprint("\nNET: HTTP & LLM layer OK!\n\n");
    } else {
        kprint("\nNET: LLM layer FAILED\n\n");
    }
}

int llm_diagnose(const char *user_question, char *reply_out, uint32_t reply_max, uint32_t timeout_ms)
{
    static char prompt_combined[4096];
    uint32_t pos = 0;

    char telemetry[1024];
    sysinfo_format_telemetry(telemetry, sizeof(telemetry));

    for (const char *p = telemetry; *p && pos < sizeof(prompt_combined) - 1; ++p) {
        prompt_combined[pos++] = *p;
    }

    const char *intro = "\nInstruccion: ";
    for (const char *p = intro; *p && pos < sizeof(prompt_combined) - 1; ++p) {
        prompt_combined[pos++] = *p;
    }

    const char *q = (user_question && *user_question) 
        ? user_question 
        : "Analiza la telemetria de MYOS. Emite directamente tu diagnostico en 3 frases en 'content'. No uses razonamiento interno.";

    for (const char *p = q; *p && pos < sizeof(prompt_combined) - 1; ++p) {
        prompt_combined[pos++] = *p;
    }

    prompt_combined[pos] = '\0';

    return llm_chat(prompt_combined, reply_out, reply_max, timeout_ms);
}


/* Las respuestas viven en http_resp_buf/payload_buf (estaticos): el cerrojo
 * cubre toda la llamada, incluida la extraccion del JSON. */
int llm_health(void)
{
    net_lock();
    int r = llm_health_unlocked();
    net_unlock();
    return r;
}

int llm_chat_json(const char *prompt, char *reply_out, uint32_t reply_max, uint32_t timeout_ms)
{
    net_lock();
    int r = llm_chat_json_unlocked(prompt, reply_out, reply_max, timeout_ms);
    net_unlock();
    return r;
}

int llm_chat(const char *prompt, char *reply_out, uint32_t reply_max, uint32_t timeout_ms)
{
    net_lock();
    int r = llm_chat_unlocked(prompt, reply_out, reply_max, timeout_ms);
    net_unlock();
    return r;
}
