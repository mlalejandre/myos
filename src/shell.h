#ifndef SOMA_SHELL_H
#define SOMA_SHELL_H

#include <stdint.h>

typedef int (*cmd_handler_t)(const char *args, char *out_buf, uint32_t max_out);

#define CMD_F_NORMAL    0
#define CMD_F_ALIAS     (1 << 0)
#define CMD_F_SAFE      (1 << 1)

struct command_entry {
    const char   *name;
    cmd_handler_t handler;
    uint32_t      flags;
    const char   *help;
} __attribute__((aligned(8)));

void shell_run(void);
int  dispatch_command(const char *cmd_line, char *out_buf, uint32_t max_out);
int  dispatch_agent_command(const char *cmd_line, char *out_buf, uint32_t max_out);
int  shell_autocomplete(const char *prefix, uint32_t plen, const char *matches[], int max_matches);
void show_somafetch(char *out_buf, uint32_t max_out);
void pci_scan(void);

#endif
