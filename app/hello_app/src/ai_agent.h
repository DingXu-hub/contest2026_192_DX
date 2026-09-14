/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ai_agent.h - on-device AI agent for the Huangshan watch.
 *
 * AI hardware track requirement: an embedded agent that is both PROACTIVE
 * (acts without being asked) and can EXECUTE (tool calls on the device).
 *
 * This implementation runs entirely on the watch (NuttX):
 *   - interaction channel : ASCII line protocol on /dev/console
 *   - tools (execution)   : run control, status, reminder timer, UI notify
 *   - proactive engine    : timer due / inactivity -> agent-initiated action
 *   - LLM access          : the agent frames a request; the PC-side relay
 *                           (tools/llm_relay.py) performs the HTTPS call and
 *                           writes the answer back over the serial line.
 */

#ifndef __AI_AGENT_H
#define __AI_AGENT_H

#include <stdint.h>
#include <stdbool.h>

#define AI_TEXT_MAX   96
#define AI_PROMPT_MAX 160

typedef struct {
    bool     active;                  /* notification on screen */
    uint32_t notify_until_ms;
    char     notify[AI_TEXT_MAX];
    char     notify_src[16];          /* proactive / answer / tool */
    char     last_prompt[AI_PROMPT_MAX];
    uint32_t last_prompt_ms;
    uint32_t req_id;
    char     last_answer[AI_TEXT_MAX];
    uint32_t last_answer_ms;
    bool     answer_pending;
    uint32_t interactions;
    uint32_t proactive_count;
    uint32_t timer_end_ms;            /* reminder countdown */
} ai_agent_t;

void  ai_agent_init(void);
void  ai_agent_set_context(void *app_ctx);
void  ai_agent_task(void);
void  ai_agent_notify(const char *src, const char *text);
void  ai_agent_dismiss(void);
ai_agent_t *ai_agent_get(void);

#endif /* __AI_AGENT_H */
