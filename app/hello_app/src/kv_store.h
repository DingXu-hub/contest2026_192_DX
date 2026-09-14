/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * kv_store.h - tiny wrapper over the NuttX key-value database (KVDB)
 * used for settings and run history persistence.
 */

#ifndef __KV_STORE_H
#define __KV_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define KV_HISTORY_MAX  8    /* number of stored past runs */

int  kv_set_int(const char *key, int32_t value);
int  kv_get_int(const char *key, int32_t *value, int32_t dflt);
int  kv_set_str(const char *key, const char *value);
int  kv_get_str(const char *key, char *buf, int buflen);

/* run history helpers: a run summary is stored under key prefix
 * "run.<idx>." with fields dist_m / sec / ascent_m */
void kv_history_push(float dist_m, uint32_t sec, float ascent_m);
int  kv_history_count(void);
int  kv_history_get(int idx, float *dist_m, uint32_t *sec, float *ascent_m);
void kv_history_clear(void);

#endif /* __KV_STORE_H */
