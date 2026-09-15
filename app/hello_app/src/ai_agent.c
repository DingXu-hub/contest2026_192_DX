/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ai_agent.c - see ai_agent.h.
 *
 * Serial line protocol (ASCII, one line per message, 1 Mbaud console):
 *
 *   PC  -> watch : "?<question>"           ask the agent (LLM)
 *                  "!<tool> [arg]"         execute a tool directly
 *                  "@LLMRESP <id> <text>"  the relay's answer
 *   watch -> PC  : "@LLMREQ <id> <prompt>" LLM request for the relay
 *                  "@TOOL <name> <result>" tool execution log
 *                  "@AI <text>"            proactive event
 *
 * Tools: !start !stop !status !timer <min> !tip !help
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>\n#include <sys/time.h>

#include "ai_agent.h"
#include "link.h"
#include "pages.h"
#include "sensor_manager.h"
#include "run_engine.h"
#include "power_manager.h"

#define AI_LINE_MAX 220

static ai_agent_t g_ai;
static app_ctx_t *g_ctx;

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void ai_send(const char *s)
{
    /* framed text when a PC gateway is listening, raw line otherwise;
     * the console fd itself is owned by link.c */
    link_send_text(s);
}

ai_agent_t *ai_agent_get(void)
{
    return &g_ai;
}

/* HTTP proxy replies (from the PC gateway) surface as a UI card */
static void http_card(const char *text)
{
    ai_agent_notify("http", text);
}

void ai_agent_init(void)
{
    memset(&g_ai, 0, sizeof(g_ai));
    link_set_http_sink(http_card);
}

void ai_agent_set_context(void *app_ctx)
{
    g_ctx = (app_ctx_t *)app_ctx;
}

void ai_agent_notify(const char *src, const char *text)
{
    strlcpy(g_ai.notify, text, sizeof(g_ai.notify));
    strlcpy(g_ai.notify_src, src, sizeof(g_ai.notify_src));
    g_ai.notify_until_ms = now_ms() + 8000;
    g_ai.active = true;
    if (g_ctx)
        g_ctx->ui.dirty = true;
}

/* hide the card early (user tapped it) */
void ai_agent_dismiss(void)
{
    g_ai.active = false;
    g_ai.notify[0] = '\0';
    if (g_ctx)
        g_ctx->ui.dirty = true;
}

/* ---------------- tools ---------------- */

static void fill_status(char *out, size_t outlen)
{
    sensor_manager_t *s = g_ctx ? g_ctx->sensors : NULL;
    run_engine_t *r = g_ctx ? &g_ctx->run : NULL;

    snprintf(out, outlen,
             "run=%d dist=%.2fkm | mag=%s yaw=%.0f",
             r ? (int)r->state : -1,
             r ? (double)(r->distance_m / 1000.0f) : 0.0,
             (s && s->mag.present && s->mag.healthy && !s->mag_dropped)
                 ? "ok" : "gyro",
             s ? (double)s->heading_deg : 0.0);
}

static void tool_status(void)
{
    char st[96];
    fill_status(st, sizeof(st));
    ai_agent_notify("tool", st);
    ai_send("@TOOL status");
    ai_send(st);
}

static void tool_start(void)
{
    if (g_ctx && g_ctx->run.state != RUN_RUNNING)
    {
        run_start(&g_ctx->run);
        if (g_ctx->sensors)
            g_ctx->run.heading_deg = sensor_get_heading(g_ctx->sensors);
        g_ctx->run_saved = false;
    }
    ai_agent_notify("tool", "Run started - go!");
    ai_send("@TOOL start ok");
}

static void tool_stop(void)
{
    char buf[96];
    if (!g_ctx)
        return;
    run_stop(&g_ctx->run);
    snprintf(buf, sizeof(buf), "Stopped: %.2f km in %u s",
             (double)(g_ctx->run.distance_m / 1000.0f),
             (unsigned)(g_ctx->run.running_ms / 1000));
    ai_agent_notify("tool", buf);
    ai_send("@TOOL stop");
    ai_send(buf);
}

static void tool_timer(int minutes)
{
    char buf[64];
    if (minutes <= 0 || minutes > 240)
        minutes = 30;
    g_ai.timer_end_ms = now_ms() + (uint32_t)minutes * 60000u;
    snprintf(buf, sizeof(buf), "Reminder in %d min", minutes);
    ai_agent_notify("tool", buf);
    ai_send("@TOOL timer");
    ai_send(buf);
}

static const char *g_help =
    "tools: !start !stop !status !timer N !tip !link !time !http URL !help | ?question";

/* ---------------- LLM bridge ---------------- */

static void llm_ask(const char *prompt)
{
    char line[AI_LINE_MAX];

    g_ai.req_id++;
    strlcpy(g_ai.last_prompt, prompt, sizeof(g_ai.last_prompt));
    g_ai.last_prompt_ms = now_ms();
    g_ai.answer_pending = true;

    snprintf(line, sizeof(line), "@LLMREQ %u %s", (unsigned)g_ai.req_id,
             prompt);
    ai_send(line);
}

/* ---------------- proactive engine ---------------- */

static void proactive_check(void)
{
    static uint32_t last_proactive;
    uint32_t t = now_ms();

    if (g_ai.timer_end_ms && t >= g_ai.timer_end_ms)
    {
        g_ai.timer_end_ms = 0;
        g_ai.proactive_count++;
        ai_agent_notify("proactive", "Reminder: time to move!");
        ai_send("@AI timer-due");
        return;
    }

    if (t > 200000u && (t - last_proactive) > 180000u &&
        (t - g_ai.last_prompt_ms) > 180000u)
    {
        last_proactive = t;
        g_ai.proactive_count++;
        llm_ask("You are a wrist-watch running coach. The user has been "
                "idle. Reply with ONE motivating tip, max 60 characters.");
        ai_send("@AI proactive-ask");
    }
}

/* ---------------- command dispatch ---------------- */

static void handle_line(char *line);

void ai_agent_feed_line(const char *line_in)
{
    char line[AI_LINE_MAX];
    strlcpy(line, line_in, sizeof(line));
    handle_line(line);
}

static void handle_line(char *line)
{
    /* Tolerate a missing command prefix: the console UART can drop the
     * first byte of a burst coming from the PC, which turned "!status"
     * into "status" (answered with help).  A bare tool name is
     * therefore treated exactly like "!name". */
    if (line[0] != '?' && line[0] != '!' && line[0] != '@')
    {
        static const char *const bare[] = { "start", "stop", "status",
                                            "timer", "tip", "link", "time", "http", "help", NULL };
        int i;

        for (i = 0; bare[i]; i++)
        {
            size_t n = strlen(bare[i]);

            if (!strncmp(line, bare[i], n) &&
                (line[n] == '\0' || line[n] == ' '))
            {
                /* the caller always leaves at least one spare byte */
                memmove(line + 1, line, strlen(line) + 1);
                line[0] = '!';
                break;
            }
        }
    }

    if (line[0] == '?')
    {
        char prompt[AI_PROMPT_MAX];
        char st[48];
        char q[90];
        g_ai.interactions++;
        strlcpy(q, line + 1, sizeof(q));
        fill_status(st, sizeof(st));
        snprintf(prompt, sizeof(prompt), "%s (watch: %s)", q, st);
        llm_ask(prompt);
        ai_agent_notify("answer", "Thinking...");
    }
    else if (line[0] == '!')
    {
        g_ai.interactions++;
        if (!strncmp(line + 1, "start", 5))
            tool_start();
        else if (!strncmp(line + 1, "stop", 4))
            tool_stop();
        else if (!strncmp(line + 1, "status", 6))
            tool_status();
        else if (!strncmp(line + 1, "timer", 5))
            tool_timer(atoi(line + 7));
        else if (!strncmp(line + 1, "link", 4))
        {
            link_print_stats();
            {
                char b[96];
                snprintf(b, sizeof(b), "link: gateway=%s",
                         link_gateway_present() ? "present" : "absent");
                ai_agent_notify("tool", b);
                ai_send("@TOOL link");
                ai_send(b);
            }
        }
        else if (!strncmp(line + 1, "time", 4))
        {
            struct timeval tv;
            time_t lt;
            struct tm tmv;
            char stamp[32];
            char b[96];

            gettimeofday(&tv, NULL);
            /* report through the *same* path the watch face uses
             * (time(NULL) + localtime) so a link-side check proves what the
             * UI will display */
            lt = time(NULL);
            localtime_r(&lt, &tmv);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
            snprintf(b, sizeof(b), "%s tz=%s t=%lu (link %s)", stamp,
                     getenv("TZ") ? getenv("TZ") : "-",
                     (unsigned long)lt,
                     link_gateway_present() ? "up" : "down");
            ai_send("@TOOL time");
            ai_send(b);
            ai_agent_notify("tool", b);
            link_send_ctrl(LINK_C_PING, NULL, 0);   /* also probe the gateway */
        }
        else if (!strncmp(line + 1, "http", 4))
        {
            const char *url = line + 6;
            while (*url == ' ')
                url++;
            if (*url == '\0')
            {
                ai_send("@TOOL http");
                ai_send("usage: !http <url>");
            }
            else if (link_http_get(url) == 0)
            {
                ai_send("@TOOL http");
                ai_send("requested");
                ai_agent_notify("tool", "fetching URL via PC gateway...");
            }
            else
            {
                ai_send("@TOOL http");
                ai_send("no gateway (start tools/gateway.py)");
                ai_agent_notify("tool", "no gateway: run tools/gateway.py");
            }
        }
        else if (!strncmp(line + 1, "tip", 3))
        {
            llm_ask("Give ONE short (max 60 chars) running-form tip.");
            ai_agent_notify("answer", "Thinking...");
        }
        else
        {
            ai_agent_notify("tool", g_help);
            ai_send("@TOOL help");
            ai_send(g_help);
        }
    }
    else if (!strncmp(line, "@LLMRESP", 8))
    {
        char *text = strchr(line + 8, ' ');
        g_ai.answer_pending = false;
        strlcpy(g_ai.last_answer, text ? text + 1 : "",
                sizeof(g_ai.last_answer));
        g_ai.last_answer_ms = now_ms();
        ai_agent_notify("answer",
                        g_ai.last_answer[0] ? g_ai.last_answer : "(empty)");
    }
}

/* ---------------- task ---------------- */

/* period work, driven by the link task every poll cycle */
void ai_agent_tick(void)
{
    if (g_ai.active && now_ms() > g_ai.notify_until_ms)
    {
        g_ai.active = false;
        if (g_ctx)
            g_ctx->ui.dirty = true;
    }

    proactive_check();
}

/* legacy entry point kept for compatibility: now only initialises and
 * announces readiness - the console is owned by link.c */
void ai_agent_task(void)
{
    ai_agent_init();
    ai_send("@AI agent-ready: type ?question or !help");

    for (;;)
    {
        usleep(200000);
        ai_agent_tick();
    }
}
