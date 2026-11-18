/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS in shared memory, so a forked child (one "boot") leaves its writes for
 * the next one.
 */
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

#include "nvs.h"

#define SLOTS 256
#define NS_MAX 4

typedef struct {
    bool used;
    char ns[16];
    char key[16];
    bool is_str;
    int32_t i;
    char s[4000];
} slot_t;

typedef struct {
    char ns[NS_MAX][16]; /* the namespaces that exist */
    slot_t slot[SLOTS];
} store_t;

static store_t *st;
static char s_open[8][16]; /* handle -> namespace */

static void ensure(void)
{
    if (!st) {
        st = mmap(NULL, sizeof(*st), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        memset(st, 0, sizeof(*st));
    }
}

static int ns_index(const char *ns)
{
    for (int i = 0; i < NS_MAX; i++) {
        if (strcmp(st->ns[i], ns) == 0) {
            return i;
        }
    }
    return -1;
}

void fake_nvs_clear(void)
{
    ensure();
    memset(st, 0, sizeof(*st));
}

int fake_nvs_keys(const char *ns)
{
    ensure();
    if (ns_index(ns) < 0) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        n += st->slot[i].used && strcmp(st->slot[i].ns, ns) == 0;
    }
    return n;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    ensure();
    if (ns_index(ns) < 0) {
        if (mode == NVS_READONLY) {
            return ESP_ERR_NOT_FOUND;
        }
        int i = ns_index("");
        strcpy(st->ns[i], ns);
    }
    for (int h = 1; h < 8; h++) {
        if (!s_open[h][0]) {
            strcpy(s_open[h], ns);
            *out = h;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t h)
{
    s_open[h][0] = '\0';
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static slot_t *find(nvs_handle_t h, const char *key, bool make)
{
    slot_t *empty = NULL;
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &st->slot[i];
        if (s->used && strcmp(s->ns, s_open[h]) == 0 && strcmp(s->key, key) == 0) {
            return s;
        }
        if (!s->used && !empty) {
            empty = s;
        }
    }
    if (!make || !empty) {
        return NULL;
    }
    memset(empty, 0, sizeof(*empty));
    empty->used = true;
    strcpy(empty->ns, s_open[h]);
    strcpy(empty->key, key);
    return empty;
}

esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out)
{
    slot_t *s = find(h, key, false);
    if (!s || s->is_str) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = s->i;
    return ESP_OK;
}

esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v)
{
    slot_t *s = find(h, key, true);
    s->is_str = false;
    s->i = v;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    int32_t v;
    esp_err_t e = nvs_get_i32(h, key, &v);
    if (e == ESP_OK) {
        *out = (uint8_t)v;
    }
    return e;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{
    return nvs_set_i32(h, key, v);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    slot_t *s = find(h, key, false);
    if (!s || !s->is_str) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t need = strlen(s->s) + 1;
    if (!out) {
        *len = need;
        return ESP_OK;
    }
    if (*len < need) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, s->s, need);
    *len = need;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v)
{
    if (strlen(v) >= sizeof(st->slot[0].s)) {
        return ESP_ERR_INVALID_SIZE;
    }
    slot_t *s = find(h, key, true);
    s->is_str = true;
    strcpy(s->s, v);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    slot_t *s = find(h, key, false);
    if (!s) {
        return ESP_ERR_NOT_FOUND;
    }
    s->used = false;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    for (int i = 0; i < SLOTS; i++) {
        if (st->slot[i].used && strcmp(st->slot[i].ns, s_open[h]) == 0) {
            st->slot[i].used = false;
        }
    }
    return ESP_OK;
}
