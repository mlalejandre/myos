#ifndef SOMA_AGENT_H
#define SOMA_AGENT_H

#include <stdint.h>

void agent_run(const char *mission);
void agent_state_load(void);
void hpatch_command(const char *arg);

extern char agent_resume_mission[512];
extern int  agent_resume_pending;

#endif
