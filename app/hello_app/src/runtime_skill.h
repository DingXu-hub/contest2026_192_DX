/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * runtime_skill.h - on-device Agent runtime skills.
 *
 * The contest's AI-hardware track asks for runtime skills that live in
 * /data/agent/skills/ and are triggered from the watch itself.  This module
 * scans that directory for small markdown skills of the form
 *
 *     name: hydrate
 *     trigger: 喝水,补水,hydrate,drink
 *     action: notify 该喝水了，补充 200 ml
 *
 * and turns them into (a) callable commands (`!skill <name>`) and (b) keyword
 * triggers for free text typed on the console / sent by the PC gateway.
 *
 * Actions are limited to the agent's existing capabilities (notify / timer /
 * status / tip / http) so a skill file can never execute arbitrary code.
 *
 * The board mounts /data as tmpfs, so rskill_init() seeds the directory from
 * a built-in table on every boot; drop extra .md files in and run
 * `!skill reload` to pick them up live.
 */

#ifndef __RUNTIME_SKILL_H
#define __RUNTIME_SKILL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RSKILL_MAX        8
#define RSKILL_NAME_LEN   24
#define RSKILL_TRIG_LEN   56
#define RSKILL_ACT_LEN    160
#define RSKILL_SRC_LEN    28

typedef struct {
    char name[RSKILL_NAME_LEN];
    char trigger[RSKILL_TRIG_LEN];   /* comma separated keywords */
    char action[RSKILL_ACT_LEN];     /* notify ... / timer N / status / tip / http URL */
    char source[RSKILL_SRC_LEN];     /* file name it came from */
} rskill_t;

/* capabilities the agent lends to skills (keeps this module decoupled) */
typedef struct {
    void (*notify)(const char *src, const char *text);
    void (*timer)(int minutes);
    void (*status)(char *out, size_t outlen);
    void (*ask)(const char *prompt);
    bool (*http)(const char *url);
} rskill_ops_t;

void rskill_set_ops(const rskill_ops_t *ops);

/* ensure the directory exists (seeding built-in examples) and load it */
int  rskill_init(void);

/* (re)scan the directory; returns the number of skills loaded */
int  rskill_load(void);

int  rskill_count(void);
const rskill_t *rskill_get(int idx);
const char *rskill_dir(void);

/* run one skill by name (as if `!skill <name>`); false when not found */
bool rskill_run(const char *name);

/* keyword scan for free text; runs the first matching skill */
bool rskill_try_trigger(const char *line);

#endif /* __RUNTIME_SKILL_H */
