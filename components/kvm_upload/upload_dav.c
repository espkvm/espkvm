/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebDAV: Nextcloud, ownCloud, a Synology or QNAP share, Apache or nginx with
 * the module on. A file is one PUT, streamed from the card, into
 * <folder>/<device name>/<VIDEO or SCREENSHOTS>/, and those two folders are
 * made with MKCOL the first time. Basic authentication, which every one of
 * those servers takes; over HTTPS the certificate is checked against the
 * built-in roots.
 */
#include "upload_priv.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "kvm_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "upload";

#define URL_CAP 400
#define CHUNK (32 * 1024)
#define TIMEOUT_MS 30000

/* The last folder made, so each file does not cost two more requests. */
static char s_made[URL_CAP];

static bool dav_ready(void)
{
    return kvm_setting_str("up_dav_url")[0] != '\0';
}

/* Percent-encode one path segment. */
static size_t enc_segment(char *out, size_t cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
    return o;
}

/* "<folder>/<device>" with no slash at the end; then "/<seg>" per call. */
static bool device_url(char *out, size_t cap)
{
    const char *base = kvm_setting_str("up_dav_url");
    size_t n = strlcpy(out, base, cap);
    while (n && out[n - 1] == '/') {
        out[--n] = '\0';
    }
    const char *host = kvm_setting_str("net_hostname");
    if (n + 2 >= cap) {
        return false;
    }
    out[n++] = '/';
    n += enc_segment(out + n, cap - n, host[0] ? host : "espkvm");
    return n + 1 < cap;
}

static void append(char *url, size_t cap, const char *seg)
{
    size_t n = strlen(url);
    if (n + 2 < cap) {
        url[n++] = '/';
        enc_segment(url + n, cap - n, seg);
    }
}

static esp_http_client_handle_t client(const char *url, esp_http_client_method_t method)
{
    const char *user = kvm_setting_str("up_dav_user");
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* harmless for plain http */
        .username = user[0] ? user : NULL,
        .password = user[0] ? kvm_setting_str("up_dav_pass") : NULL,
        .auth_type = user[0] ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .buffer_size_tx = 1024,
    };
    return esp_http_client_init(&cfg);
}

/* What a status means, in words for the console. */
static upload_res_t verdict(int status, const char *what)
{
    upload_res_t r = {.ok = status >= 200 && status < 300};
    if (r.ok) {
        return r;
    }
    r.retry = status <= 0 || status == 408 || status == 429 || (status >= 500 && status != 507);
    if (status == 401 || status == 403) {
        snprintf(r.message, sizeof(r.message), "the server refused the user name or password");
    } else if (status == 404 || status == 409) {
        snprintf(r.message, sizeof(r.message), "the folder in the address does not exist on the server");
    } else if (status == 507) {
        snprintf(r.message, sizeof(r.message), "the server is out of space");
    } else if (status == 413) {
        snprintf(r.message, sizeof(r.message), "the server does not take a file this big");
    } else if (status <= 0) {
        snprintf(r.message, sizeof(r.message), "could not reach the server");
    } else {
        snprintf(r.message, sizeof(r.message), "%s: HTTP %d", what, status);
    }
    return r;
}

/* MKCOL: 201 made it, 405 says it is there already. */
static upload_res_t mkcol(const char *url)
{
    esp_http_client_handle_t c = client(url, HTTP_METHOD_MKCOL);
    if (!c) {
        return verdict(-1, "folder");
    }
    /* A refused password ends perform() in an error after the 401 came back:
       the status still says what happened. */
    const esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    if (err != ESP_OK && status <= 0) {
        status = -1;
    }
    esp_http_client_cleanup(c);
    return verdict(status == 405 ? 201 : status, "making the folder");
}

/* The device's folder and the one under it, unless made already. */
static upload_res_t make_folders(const char *sub)
{
    char url[URL_CAP];
    if (!device_url(url, sizeof(url))) {
        return (upload_res_t){.message = "the address is too long"};
    }
    if (sub) {
        append(url, sizeof(url), sub);
    }
    if (strcmp(url, s_made) == 0) {
        return (upload_res_t){.ok = true};
    }
    char dev[URL_CAP];
    device_url(dev, sizeof(dev));
    upload_res_t r = mkcol(dev);
    if (r.ok && sub) {
        r = mkcol(url);
    }
    if (r.ok) {
        strlcpy(s_made, url, sizeof(s_made));
    }
    return r;
}

static const char *content_type(const char *rel)
{
    const char *dot = strrchr(rel, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".ts") == 0) {
        return "video/mp2t";
    }
    if (strcmp(dot, ".mp4") == 0) {
        return "video/mp4";
    }
    if (strcmp(dot, ".jpg") == 0) {
        return "image/jpeg";
    }
    return "text/plain";
}

static upload_res_t dav_put(const char *full, const char *rel, uint64_t size, upload_progress_fn progress)
{
    /* "VIDEO/x.ts": the folder, then the name. */
    char sub[32] = "";
    const char *slash = strchr(rel, '/');
    const char *name = rel;
    if (slash && (size_t)(slash - rel) < sizeof(sub)) {
        memcpy(sub, rel, (size_t)(slash - rel));
        sub[slash - rel] = '\0';
        name = slash + 1;
    }
    upload_res_t r = make_folders(sub[0] ? sub : NULL);
    if (!r.ok) {
        s_made[0] = '\0';
        return r;
    }
    char url[URL_CAP];
    device_url(url, sizeof(url));
    if (sub[0]) {
        append(url, sizeof(url), sub);
    }
    append(url, sizeof(url), name);

    FILE *f = fopen(full, "rb");
    char *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t c = (f && chunk) ? client(url, HTTP_METHOD_PUT) : NULL;
    if (!c) {
        if (f) {
            fclose(f);
        }
        free(chunk);
        snprintf(r.message, sizeof(r.message), f ? "out of memory" : "could not open the file on the card");
        r.retry = f != NULL; /* memory is often back in a moment */
        return r;
    }
    esp_http_client_set_header(c, "Content-Type", content_type(rel));
    int status = -1;
    if (size <= INT32_MAX && esp_http_client_open(c, (int)size) == ESP_OK) {
        uint64_t sent = 0;
        bool go = true;
        size_t n;
        while (go && (n = fread(chunk, 1, CHUNK, f)) > 0) {
            go = esp_http_client_write(c, chunk, (int)n) == (int)n;
            sent += n;
            go = go && progress(sent);
        }
        if (go && sent == size && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
        } else if (go) {
            ESP_LOGW(TAG, "%s: sent %llu of %llu bytes", rel, (unsigned long long)sent,
                     (unsigned long long)size);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    fclose(f);
    free(chunk);
    r = verdict(status, "uploading");
    if (!r.ok) {
        s_made[0] = '\0'; /* a folder deleted on the server meanwhile is made again */
    }
    return r;
}

static upload_res_t dav_test(void)
{
    s_made[0] = '\0';
    upload_res_t r = make_folders(NULL);
    if (r.ok) {
        snprintf(r.message, sizeof(r.message), "the server took it: the device's folder is there");
    }
    return r;
}

const upload_dest_t upload_dest_dav = {
    .name = "WebDAV",
    .ready = dav_ready,
    .put = dav_put,
    .test = dav_test,
};
