/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * kv_store.c - simple text key-value persistence on the filesystem
 * (/data/app_kv.txt).  On the host simulator /data maps to the host /tmp
 * through hostfs; on the target it is the writable data partition.
 */

#include "kv_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

/* NOTE: /data is tmpfs on this board (RAM, lost on reboot).  A littlefs
 * on the NOR FS_REGION (/dev/config0) was tried and abandoned: runtime
 * NOR write/erase hard-faults inside the SiFli HAL on this XIP build.
 * So the KV store is session-scoped; the mag calibration that matters is
 * baked into the firmware as compile-time defaults (see mag_mmc5603.c)
 * and the KV copy only overrides it until the next reboot. */
#define KV_FILE    "/data/app_kv.txt"
#define KV_MAXSIZE 4096

/* Task stacks are tight (sensor task: 6 KiB), so the KV scratch buffers
 * live in BSS instead of on the caller's stack.  The mutex serialises
 * access because the UI, sensor and BT tasks may all hit the store. */

static pthread_mutex_t s_kv_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_kv_read[KV_MAXSIZE];   /* kv_get_str scratch          */
static char s_kv_buf[KV_MAXSIZE];    /* kv_set_str: current content */
static char s_kv_out[KV_MAXSIZE];    /* kv_set_str: rewritten file  */

static int kv_load(char *buf, int buflen)
{
    FILE *f = fopen(KV_FILE, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static int kv_save(const char *buf)
{
    FILE *f = fopen(KV_FILE, "w");
    if (!f)
        return -1;
    fputs(buf, f);
    fclose(f);
    return 0;
}

/* find the value for key in data; returns NULL if absent */
static const char *kv_find(const char *data, const char *key)
{
    int keylen = (int)strlen(key);
    const char *p = data;

    if (!p)
        return NULL;

    while (*p)
    {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > keylen && strncmp(p, key, keylen) == 0 && p[keylen] == '=')
            return p + keylen + 1;
        p += len + (nl ? 1 : 0);
    }
    return NULL;
}

int kv_set_str(const char *key, const char *value)
{
    char line[160];
    const char *val;
    int n;
    int ret;

    snprintf(line, sizeof(line), "%s=%s\n", key, value);

    pthread_mutex_lock(&s_kv_lock);

    n = kv_load(s_kv_buf, sizeof(s_kv_buf));
    if (n < 0)
        s_kv_buf[0] = '\0';

    val = kv_find(s_kv_buf, key);
    if (val)
    {
        /* replace: copy everything up to val, then line, then rest */
        const char *vstart = val;
        const char *vend = strchr(vstart, '\n');
        const char *rest;
        size_t prefix;
        size_t rest_len;
        char *o = s_kv_out;

        if (!vend)
            vend = vstart + strlen(vstart);
        rest = *vend ? vend + 1 : vend;
        prefix = (size_t)(vstart - s_kv_buf);
        rest_len = strlen(rest);

        memcpy(o, s_kv_buf, prefix);
        o += prefix;
        memcpy(o, line, strlen(line));
        o += strlen(line);
        memcpy(o, rest, rest_len + 1);
    }
    else
    {
        /* append */
        char *o = s_kv_out;

        memcpy(o, s_kv_buf, strlen(s_kv_buf) + 1);
        o += strlen(s_kv_buf);
        memcpy(o, line, strlen(line) + 1);
    }

    ret = kv_save(s_kv_out);

    pthread_mutex_unlock(&s_kv_lock);
    return ret;
}

int kv_get_str(const char *key, char *buf, int buflen)
{
    const char *val;
    int vlen;
    int n;
    int ret = -1;

    pthread_mutex_lock(&s_kv_lock);

    n = kv_load(s_kv_read, sizeof(s_kv_read));
    if (n >= 0)
    {
        val = kv_find(s_kv_read, key);
        if (val)
        {
            const char *nl = strchr(val, '\n');

            vlen = nl ? (int)(nl - val) : (int)strlen(val);
            if (vlen >= buflen)
                vlen = buflen - 1;
            memcpy(buf, val, vlen);
            buf[vlen] = '\0';
            ret = 0;
        }
    }

    pthread_mutex_unlock(&s_kv_lock);
    return ret;
}

int kv_set_int(const char *key, int32_t value)
{
    char v[24];
    snprintf(v, sizeof(v), "%ld", (long)value);
    return kv_set_str(key, v);
}

int kv_get_int(const char *key, int32_t *value, int32_t dflt)
{
    char buf[24];
    if (kv_get_str(key, buf, sizeof(buf)) >= 0)
    {
        *value = (int32_t)strtol(buf, NULL, 10);
        return 0;
    }
    *value = dflt;
    return -1;
}

/* ------------------------------------------------------------------ *
 * run history (ring of KV_HISTORY_MAX)
 * ------------------------------------------------------------------ */

static void hist_key(char *out, int outlen, int idx, const char *field)
{
    snprintf(out, outlen, "run.%d.%s", idx, field);
}

static int hist_get_int(int idx, const char *field, int32_t *v)
{
    char key[32];
    hist_key(key, sizeof(key), idx, field);
    return kv_get_int(key, v, 0);
}

static void hist_set_int(int idx, const char *field, int32_t v)
{
    char key[32];
    hist_key(key, sizeof(key), idx, field);
    kv_set_int(key, v);
}

void kv_history_push(float dist_m, uint32_t sec, float ascent_m)
{
    int32_t count = 0;
    int i;

    kv_get_int("run.count", &count, 0);
    if (count < 0) count = 0;
    if (count > KV_HISTORY_MAX) count = KV_HISTORY_MAX;

    for (i = count - 1; i >= 0; i--)
    {
        int32_t d, s, a;
        if (hist_get_int(i, "d", &d) < 0)
            continue;
        hist_get_int(i, "s", &s);
        hist_get_int(i, "a", &a);
        hist_set_int(i + 1, "d", d);
        hist_set_int(i + 1, "s", s);
        hist_set_int(i + 1, "a", a);
    }

    hist_set_int(0, "d", (int32_t)(dist_m * 10.0f));
    hist_set_int(0, "s", (int32_t)sec);
    hist_set_int(0, "a", (int32_t)ascent_m);

    count++;
    kv_set_int("run.count", count);
}

int kv_history_count(void)
{
    int32_t count = 0;
    kv_get_int("run.count", &count, 0);
    if (count < 0) count = 0;
    if (count > KV_HISTORY_MAX) count = KV_HISTORY_MAX;
    return count;
}

int kv_history_get(int idx, float *dist_m, uint32_t *sec, float *ascent_m)
{
    int32_t d = 0, s = 0, a = 0;

    if (idx < 0 || idx >= KV_HISTORY_MAX)
        return -1;

    if (hist_get_int(idx, "d", &d) < 0)
        return -1;
    hist_get_int(idx, "s", &s);
    hist_get_int(idx, "a", &a);

    *dist_m = (float)d / 10.0f;
    *sec = (uint32_t)s;
    *ascent_m = (float)a;
    return 0;
}

void kv_history_clear(void)
{
    kv_set_int("run.count", 0);
}
