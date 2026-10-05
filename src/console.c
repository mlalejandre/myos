volatile int console_muted = 0;
#include "thread.h"
#include "console.h"
#include "io.h"

#define COM1 0x3F8

static volatile uint16_t *const VGA_BUF = (uint16_t *)0xB8000;
static uint32_t vga_row = 0;
static uint32_t vga_col = 0;

static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(vga_row * 80 + vga_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void console_clear(void)
{
    for (uint32_t i = 0; i < 80 * 25; ++i) {
        VGA_BUF[i] = 0x0720;
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

static void vga_scroll(void)
{
    while (vga_row >= 25) {
        for (int i = 0; i < 24 * 80; ++i) {
            VGA_BUF[i] = VGA_BUF[i + 80];
        }
        for (int i = 24 * 80; i < 25 * 80; ++i) {
            VGA_BUF[i] = 0x0720;
        }
        vga_row--;
    }
}

static uint8_t vga_attr = 0x07;
static int ansi_state = 0;
static int ansi_arg1 = 0;
static int ansi_arg2 = 0;
static int ansi_has_arg2 = 0;

static void vga_putc(char c)
{
    /* Parser bare-metal de secuencias de color ANSI (\033[...m) */
    if (ansi_state == 0) {
        if ((uint8_t)c == 27) { /* ESC */
            ansi_state = 1;
            return;
        }
    } else if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_arg1 = 0;
            ansi_arg2 = 0;
            ansi_has_arg2 = 0;
            return;
        }
        ansi_state = 0;
    } else if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (!ansi_has_arg2) {
                ansi_arg1 = ansi_arg1 * 10 + (c - '0');
            } else {
                ansi_arg2 = ansi_arg2 * 10 + (c - '0');
            }
            return;
        } else if (c == ';') {
            ansi_has_arg2 = 1;
            return;
        } else if (c == 'm') {
            int bold = (ansi_arg1 == 1 || (ansi_has_arg2 && ansi_arg2 == 1));
            int code = (ansi_arg1 == 1 && ansi_has_arg2) ? ansi_arg2 : ansi_arg1;

            if (ansi_arg1 == 0 && !ansi_has_arg2) {
                vga_attr = 0x07; /* Reset a gris estandar */
            } else {
                uint8_t fg = 0x07;
                if (code == 30) fg = 0x00;      /* Negro */
                else if (code == 31) fg = 0x04; /* Rojo */
                else if (code == 32) fg = 0x02; /* Verde */
                else if (code == 33) fg = 0x06; /* Amarillo */
                else if (code == 34) fg = 0x01; /* Azul */
                else if (code == 35) fg = 0x05; /* Magenta */
                else if (code == 36) fg = 0x03; /* Cian */
                else if (code == 37) fg = 0x07; /* Blanco */

                if (bold) fg |= 0x08; /* Intensidad alta / brillante */
                vga_attr = fg;
            }
            ansi_state = 0;
            return;
        } else {
            ansi_state = 0;
            return;
        }
    }

    if (c == '\r') {
        vga_col = 0;
    } else if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUF[vga_row * 80 + vga_col] = (uint16_t)((vga_attr << 8) | ' ');
        }
    } else if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
        VGA_BUF[vga_row * 80 + vga_col] = (uint16_t)((vga_attr << 8) | (uint8_t)c);
        vga_col++;
        if (vga_col >= 80) {
            vga_col = 0;
            vga_row++;
        }
    }
    vga_scroll();
    vga_update_cursor();
}

void kputc(char c)
{
    if (console_muted) return;
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);

    vga_putc(c);
}

void kprint(const char *s)
{
    for (; *s; ++s) {
        kputc(*s);
    }
}

static void put_nibble(uint8_t v)
{
    v &= 0x0F;
    kputc(v < 10 ? (char)('0' + v) : (char)('A' + v - 10));
}

void kprint_hex8(uint8_t value)
{
    put_nibble((uint8_t)(value >> 4));
    put_nibble(value);
}

void kprint_hex16(uint16_t value)
{
    kprint_hex8((uint8_t)(value >> 8));
    kprint_hex8((uint8_t)value);
}

void kprint_hex32(uint32_t value)
{
    kprint_hex16((uint16_t)(value >> 16));
    kprint_hex16((uint16_t)value);
}

void kprint_dec(uint32_t value)
{
    char tmp[10];
    int n = 0;

    if (value == 0) {
        kputc('0');
        return;
    }

    while (value != 0) {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0) {
        kputc(tmp[--n]);
    }
}

void kprint_mac(const uint8_t *mac)
{
    for (int i = 0; i < 6; ++i) {
        kprint_hex8(mac[i]);
        if (i != 5) kputc(':');
    }
}

void kprint_ip(const uint8_t *ip)
{
    for (int i = 0; i < 4; ++i) {
        kprint_dec(ip[i]);
        if (i != 3) kputc('.');
    }
}

/* ====================================================================
 * Controlador de Teclado PS/2 (Scan Code Set 1 - IRQ1)
 * ==================================================================== */
#define KBD_BUF_SIZE 128
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;
static int shift_active = 0;
static int caps_active = 0;
static int ctrl_active = 0;
static int e0_prefix = 0;

static const char kbd_us_lower[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0, /* Ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* LShift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, /* RShift */
    '*', 0, /* Alt */ ' ', 0 /* CapsLock */
};

static const char kbd_us_upper[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0, /* Ctrl */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, /* LShift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, /* RShift */
    '*', 0, /* Alt */ ' ', 0 /* CapsLock */
};

void keyboard_irq_handler(void)
{
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }
    if (e0_prefix) {
        e0_prefix = 0;
        char k = 0;
        if (sc == 0x48) k = 16;      /* Flecha Arriba -> Ctrl+P */
        else if (sc == 0x50) k = 14; /* Flecha Abajo  -> Ctrl+N */
        else if (sc == 0x4B) k = 2;  /* Flecha Izq    -> Ctrl+B */
        else if (sc == 0x4D) k = 6;  /* Flecha Der    -> Ctrl+F */
        else if (sc == 0x47) k = 1;  /* Home          -> Ctrl+A */
        else if (sc == 0x4F) k = 5;  /* End           -> Ctrl+E */
        if (k != 0) {
            uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buf[kbd_head] = k;
                kbd_head = next;
            }
        }
        return;
    }
    if (sc == 0x1D) {
        ctrl_active = 1;
        return;
    }
    if (sc == 0x9D) {
        ctrl_active = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) {
        shift_active = 1;
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift_active = 0;
        return;
    }
    if (sc == 0x3A) {
        caps_active = !caps_active;
        return;
    }
    if (sc & 0x80) {
        return; /* Ignorar key release */
    }
    if (sc < sizeof(kbd_us_lower)) {
        char ch = shift_active ? kbd_us_upper[sc] : kbd_us_lower[sc];
        if (ctrl_active) {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
            else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 1);
        } else if (caps_active) {
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        }
        if (ch != 0) {
            uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buf[kbd_head] = ch;
                kbd_head = next;
            }
        }
    }
}

int kgetc_ready(void)
{
    if (kbd_head != kbd_tail) {
        return 1;
    }
    return (inb(COM1 + 5) & 0x01) != 0;
}

char kgetc(void)
{
    while (!kgetc_ready()) {
        thread_yield();
        __asm__ volatile ("sti; hlt");   /* el tick de 1 kHz nos despierta */
    }
    if (kbd_head != kbd_tail) {
        char ch = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
        return ch;
    }
    return (char)inb(COM1);
}

/* == MOTOR READLINE BARE-METAL == */
#define CMD_HISTORY_MAX 16
#define CMD_LINE_MAX    128

static char cmd_history[CMD_HISTORY_MAX][CMD_LINE_MAX];
static int history_count = 0;
static int history_tail = 0;

static void history_add(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') return;

    if (history_count > 0) {
        int last_idx = (history_tail - 1 + CMD_HISTORY_MAX) % CMD_HISTORY_MAX;
        int same = 1;
        for (int i = 0; cmd[i] || cmd_history[last_idx][i]; ++i) {
            if (cmd[i] != cmd_history[last_idx][i]) { same = 0; break; }
        }
        if (same) return;
    }

    int idx = history_tail;
    uint32_t i = 0;
    while (cmd[i] && i < CMD_LINE_MAX - 1) {
        cmd_history[idx][i] = cmd[i];
        i++;
    }
    cmd_history[idx][i] = '\0';

    history_tail = (history_tail + 1) % CMD_HISTORY_MAX;
    if (history_count < CMD_HISTORY_MAX) history_count++;
}

static const char *const shell_dict[] = {
    "help", "creador", "status", "uptime", "sleep", "date", "time",
    "free", "df", "tree", "cp", "mv", "touch", "head", "tail", "wc",
    "grep", "nano", "edit", "ls", "cat", "write", "rm", "fs-sync",
    "fs-format", "stats", "mem", "heap", "pmm", "vmm", "arp", "pci",
    "ping", "dns", "curl", "ps", "threads", "spawn", "bg", "kill",
    "test_suite", "health", "llm", "llm-diag", "soma", "somafetch", "neofetch", "fetch", "myos", "agent",
    "echo", "hexdump", "xxd", "reboot", "poweroff", "shutdown", "halt",
    "src_ls", "src_cat", "src_grep", "clear", "cls", 0
};

#define KEY_ACT_UP    1001
#define KEY_ACT_DOWN  1002
#define KEY_ACT_LEFT  1003
#define KEY_ACT_RIGHT 1004
#define KEY_ACT_HOME  1005
#define KEY_ACT_END   1006
#define KEY_ACT_TAB   '\t'
#define KEY_ACT_ENTER '\n'
#define KEY_ACT_BS    '\b'

static int read_key_action(void)
{
    char c = kgetc();
    if (c == 16) return KEY_ACT_UP;
    if (c == 14) return KEY_ACT_DOWN;
    if (c == 2)  return KEY_ACT_LEFT;
    if (c == 6)  return KEY_ACT_RIGHT;
    if (c == 1)  return KEY_ACT_HOME;
    if (c == 5)  return KEY_ACT_END;

    if (c == 27) { /* ESC o secuencia ANSI */
        if (kgetc_ready()) {
            char c2 = kgetc();
            if (c2 == '[') {
                char c3 = kgetc();
                if (c3 == 'A') return KEY_ACT_UP;
                if (c3 == 'B') return KEY_ACT_DOWN;
                if (c3 == 'C') return KEY_ACT_RIGHT;
                if (c3 == 'D') return KEY_ACT_LEFT;
                if (c3 == 'H') return KEY_ACT_HOME;
                if (c3 == 'F') return KEY_ACT_END;
                if (c3 == '1' || c3 == '4') {
                    if (kgetc_ready()) {
                        char c4 = kgetc();
                        if (c4 == '~') {
                            if (c3 == '1') return KEY_ACT_HOME;
                            if (c3 == '4') return KEY_ACT_END;
                        }
                    }
                }
            }
        }
        return 27;
    }

    if (c == '\r') return KEY_ACT_ENTER;
    if (c == 0x7F || c == 8) return KEY_ACT_BS;

    return (int)(uint8_t)c;
}

static void redraw_prompt_line(const char *line, uint32_t len, uint32_t cursor, uint32_t prev_len)
{
    kputc('\r');
    kprint("\033[1;36msoma\033[1;32m>\033[0m ");

    for (uint32_t i = 0; i < len; ++i) {
        kputc(line[i]);
    }

    uint32_t max_v = len;
    if (prev_len > len) {
        for (uint32_t i = len; i < prev_len; ++i) kputc(' ');
        max_v = prev_len;
    }

    uint32_t steps_back = max_v - cursor;
    if (steps_back > 0) {
        if (vga_col >= steps_back) vga_col -= steps_back;
        else vga_col = 0;
        vga_update_cursor();

        for (uint32_t i = 0; i < steps_back; ++i) {
            while ((inb(COM1 + 5) & 0x20) == 0) {}
            outb(COM1, '\b');
        }
    }
}

int kgetline(char *buf, uint32_t max)
{
    if (!buf || max == 0) return 0;

    char line[CMD_LINE_MAX];
    char temp_line[CMD_LINE_MAX];
    uint32_t len = 0;
    uint32_t cursor = 0;
    uint32_t prev_len = 0;
    int hist_idx = -1;

    line[0] = '\0';
    temp_line[0] = '\0';

    for (;;) {
        int act = read_key_action();

        if (act == KEY_ACT_ENTER) {
            kputc('\r');
            kputc('\n');
            line[len] = '\0';
            history_add(line);

            uint32_t copy_n = (len < max - 1) ? len : max - 1;
            for (uint32_t i = 0; i < copy_n; ++i) buf[i] = line[i];
            buf[copy_n] = '\0';
            return (int)copy_n;
        }

        if (act == KEY_ACT_BS) {
            if (cursor > 0) {
                prev_len = len;
                for (uint32_t i = cursor - 1; i < len; ++i) {
                    line[i] = line[i + 1];
                }
                cursor--;
                len--;
                redraw_prompt_line(line, len, cursor, prev_len);
            }
            continue;
        }

        if (act == KEY_ACT_LEFT) {
            if (cursor > 0) {
                cursor--;
                prev_len = len;
                redraw_prompt_line(line, len, cursor, prev_len);
            }
            continue;
        }

        if (act == KEY_ACT_RIGHT) {
            if (cursor < len) {
                cursor++;
                prev_len = len;
                redraw_prompt_line(line, len, cursor, prev_len);
            }
            continue;
        }

        if (act == KEY_ACT_HOME) {
            if (cursor > 0) {
                cursor = 0;
                prev_len = len;
                redraw_prompt_line(line, len, cursor, prev_len);
            }
            continue;
        }

        if (act == KEY_ACT_END) {
            if (cursor < len) {
                cursor = len;
                prev_len = len;
                redraw_prompt_line(line, len, cursor, prev_len);
            }
            continue;
        }

        if (act == KEY_ACT_UP) {
            if (history_count == 0) continue;
            prev_len = len;

            if (hist_idx == -1) {
                for (uint32_t i = 0; i <= len; ++i) temp_line[i] = line[i];
                hist_idx = history_count - 1;
            } else if (hist_idx > 0) {
                hist_idx--;
            }

            int start_idx = (history_tail - history_count + CMD_HISTORY_MAX) % CMD_HISTORY_MAX;
            int target_idx = (start_idx + hist_idx) % CMD_HISTORY_MAX;
            len = 0;
            while (cmd_history[target_idx][len] && len < sizeof(line) - 1) {
                line[len] = cmd_history[target_idx][len];
                len++;
            }
            line[len] = '\0';
            cursor = len;
            redraw_prompt_line(line, len, cursor, prev_len);
            continue;
        }

        if (act == KEY_ACT_DOWN) {
            if (hist_idx == -1) continue;
            prev_len = len;

            if (hist_idx < history_count - 1) {
                hist_idx++;
                int start_idx = (history_tail - history_count + CMD_HISTORY_MAX) % CMD_HISTORY_MAX;
                int target_idx = (start_idx + hist_idx) % CMD_HISTORY_MAX;
                len = 0;
                while (cmd_history[target_idx][len] && len < sizeof(line) - 1) {
                    line[len] = cmd_history[target_idx][len];
                    len++;
                }
                line[len] = '\0';
                cursor = len;
            } else {
                hist_idx = -1;
                len = 0;
                while (temp_line[len] && len < sizeof(line) - 1) {
                    line[len] = temp_line[len];
                    len++;
                }
                line[len] = '\0';
                cursor = len;
            }
            redraw_prompt_line(line, len, cursor, prev_len);
            continue;
        }

        if (act == KEY_ACT_TAB) {
            int has_space = 0;
            for (uint32_t i = 0; i < cursor; ++i) {
                if (line[i] == ' ') { has_space = 1; break; }
            }
            if (!has_space) {
                const char *matches[16];
                int match_count = 0;

                for (int i = 0; shell_dict[i]; ++i) {
                    int prefix_ok = 1;
                    for (uint32_t j = 0; j < cursor; ++j) {
                        if (shell_dict[i][j] != line[j]) { prefix_ok = 0; break; }
                    }
                    if (prefix_ok && match_count < 16) {
                        matches[match_count++] = shell_dict[i];
                    }
                }

                if (match_count == 1) {
                    prev_len = len;
                    const char *m = matches[0];
                    len = 0;
                    while (m[len] && len < sizeof(line) - 2) {
                        line[len] = m[len];
                        len++;
                    }
                    line[len++] = ' ';
                    line[len] = '\0';
                    cursor = len;
                    redraw_prompt_line(line, len, cursor, prev_len);
                } else if (match_count > 1) {
                    kprint("\n");
                    for (int m = 0; m < match_count; ++m) {
                        kprint("  ");
                        kprint(matches[m]);
                    }
                    kprint("\n");
                    prev_len = len;
                    redraw_prompt_line(line, len, cursor, prev_len);
                }
            }
            continue;
        }

        /* Caracter imprimible: directo al final o insercion en caliente en medio */
        if ((uint8_t)act >= 32 && (uint8_t)act < 127) {
            if (len < sizeof(line) - 2) {
                if (cursor == len) {
                    line[len] = (char)act;
                    len++;
                    cursor++;
                    line[len] = '\0';
                    kputc((char)act);
                } else {
                    prev_len = len;
                    for (uint32_t i = len + 1; i > cursor; --i) {
                        line[i] = line[i - 1];
                    }
                    line[cursor] = (char)act;
                    len++;
                    cursor++;
                    line[len] = '\0';
                    redraw_prompt_line(line, len, cursor, prev_len);
                }
            }
        }
    }
}
