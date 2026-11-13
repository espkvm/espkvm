/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server.h"

#include "sdkconfig.h" /* the CONFIG_ guards below are read before any of them */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "kvm_ota_trial.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

/* espcoredump only puts its header on the include path when core dumps are
 * enabled, so a build with them off must not ask for it. Everything that uses
 * it is behind the same guard. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "esp_https_server.h"
#include "esp_tls.h"
#include "esp_netif.h"

#include "lwip/sockets.h"

#include "cJSON.h"

#include "http_recv.h"

#include "capture.h"
#include "screentext.h"
#include "screentext_store.h"
#include "ethernet.h"
#include "kvm_ipv6.h"
#include "wifi.h"
#include "coproc.h"
#include "kvm_rtc.h"
#include "kvm_atx.h"
#include "kvm_mqtt.h"
#include "kvm_ts.h"
#include "kvm_wg.h"
#include "kvm_auth.h"
#include "fw_install.h"
#include "kvm_notify.h"
#include "kvm_sched.h"
#include "runbook.h"
#include "kvm_board_header.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "kvm_profiles.h"
#include "kvm_record.h"
#include "web_priv.h"
#include "http_priv.h"
#include "kvm_storage.h"
#include "kvm_thermal.h"
#include "kvm_tls.h"
#include "kvm_acme.h"
#include "kvm_tools.h"
#include "kvm_proto.h"
#include "usb_hid.h"
#include "video_frame.h"


static const char *TAG = "web";

/*
 * Headers every answer carries.
 *
 * Applied once, in register_route() below, so a handler that sends its own body
 * cannot forget them - which is what happened while they were a call each
 * handler had to remember.
 *
 * The console is one self-contained page - nothing is LOADED from anywhere else,
 * so the policy can be strict about where content comes from. The one thing it
 * has to allow is inline script and style, because the build inlines them into
 * that single file. What it buys is the rest: no framing (a KVM console inside
 * somebody else's invisible frame, with a "force off" button under the
 * pointer), no remote script, nowhere to send a form, and no guessing at
 * content types.
 *
 * `connect-src` is the exception, and it has to be. The page does reach outside
 * itself: it reads the update manifest, and it lists what the project has
 * published so an earlier release can be picked. An earlier version of this
 * header said the console fetched nothing at all and left `connect-src` at
 * 'self' - which quietly broke the update check with "Failed to fetch", found on
 * hardware. It cannot be a fixed list of hosts either: the manifest URL is a
 * setting, so it may be a fork or a file server inside the operator's own
 * network. Allowing https: costs little of what this header is here for - none
 * of the four things above depends on it.
 */
void kvm_web_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                       "style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; "
                       "media-src 'self' blob:; connect-src 'self' https: ws: wss:; "
                       "font-src 'self' data:; object-src 'none'; base-uri 'none'; "
                       "form-action 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    /* The same refusal to be framed, for a browser too old to read the policy
       above. It says nothing the CSP does not, and costs a few bytes. */
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
}

/** Send a malloc'd JSON document and free it. NULL means the generator failed. */

esp_err_t send_json_owned(httpd_req_t *req, char *json)
{
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[192];
    /* cJSON for a single short field is not worth the allocation, but the message
     * still has to survive quoting. */
    size_t o = 0;
    body[o++] = '{';
    o += (size_t)snprintf(body + o, sizeof(body) - o, "\"error\":\"");
    for (const char *p = message; *p && o < sizeof(body) - 8; p++) {
        if (*p == '"' || *p == '\\') {
            body[o++] = '\\';
        }
        body[o++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    o += (size_t)snprintf(body + o, sizeof(body) - o, "\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, body, o);
}

esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Read a small JSON request body. Caller frees with cJSON_Delete. NULL on any
 * problem (missing, too large, malformed). */
cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 4096) {
        return NULL;
    }
    char *buf = malloc((size_t)req->content_len + 1u);
    if (!buf) {
        return NULL;
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, (size_t)(req->content_len - got));
        if (kvm_recv_stalled(n) && ++stalls <= KVM_RECV_MAX_STALLS) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        stalls = 0;
        got += n;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/*
 * The picture, to a session or to the viewing token.
 *
 * These three - the stream, one frame, the figures that describe them - are the
 * only endpoints a token opens, and they are all read-only. Everything else
 * goes on asking for a session, which is what keeps a dashboard credential from
 * being a way to type at somebody's server.
 */
bool viewing_allowed(httpd_req_t *req, esp_err_t *out)
{
    if (kvm_auth_check(req) || kvm_auth_token_ok(req)) {
        return true;
    }
    *out = kvm_auth_challenge(req);
    return false;
}

/* The console is embedded already compressed: it costs a third of the flash
 * and a third of the transfer, and the browser does the decompression. */
extern const char index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const char index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");
/* PWA assets, so the console installs and runs full-screen from a home screen. */
extern const char manifest_start[] asm("_binary_manifest_webmanifest_start");
extern const char manifest_end[] asm("_binary_manifest_webmanifest_end");
extern const char sw_js_start[] asm("_binary_sw_js_start");
extern const char sw_js_end[] asm("_binary_sw_js_end");
extern const char icon_192_start[] asm("_binary_icon_192_png_start");
extern const char icon_192_end[] asm("_binary_icon_192_png_end");
extern const char icon_512_start[] asm("_binary_icon_512_png_start");
extern const char icon_512_end[] asm("_binary_icon_512_png_end");

httpd_handle_t s_httpd;
/** A copy of the server's own public certificate (PEM), for the download
 *  endpoint that lets an operator trust this device. NULL when TLS is off. */
static char *s_cert_pem;
/** Port 80, answering only with redirects while TLS is on. */
static httpd_handle_t s_redirect_httpd;

static esp_err_t root_get(httpd_req_t *req)
{
    /* The console is embedded in the firmware, so its identity is the firmware's:
     * version plus a few bytes of the app's ELF hash, which changes on any build.
     * Serving it "no-cache" with this ETag means a browser (or the PWA's service
     * worker) always revalidates and can never keep an old console against new
     * firmware after an OTA - yet an unchanged reload costs only a 304, not a
     * fresh 57 KB download. */
    const esp_app_desc_t *app = esp_app_get_description();
    char etag[96];
    snprintf(etag, sizeof(etag), "\"%s-%02x%02x%02x%02x\"", app->version,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2],
             app->app_elf_sha256[3]);

    char inm[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, NULL, 0);
    }

    const size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    return httpd_resp_send(req, index_html_gz_start, len);
}

/*
 * The device's CA certificate, so an operator can import it as a trust anchor
 * and trust this device - which is what turns wss and the H.264 decoder on in a
 * browser. A self-signed leaf cannot be trusted this way; a CA can. No
 * authentication: it is public (a trust anchor, not a secret), and needing it is
 * a precondition to logging in comfortably. The private key is never served.
 * Also offered on the plain port-80 server so it can be fetched without first
 * clicking through the very warning it removes.
 */
static esp_err_t cert_get(httpd_req_t *req)
{
    if (!s_cert_pem) {
        return send_json_error(req, "404 Not Found", "no certificate (TLS is off)");
    }
    /* Serve it as a CA certificate (not a generic PEM blob) so a phone routes it
     * to the "install CA certificate" flow and names it by its subject. */
    httpd_resp_set_type(req, "application/x-x509-ca-cert");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"espkvm-ca.crt\"");
    return httpd_resp_sendstr(req, s_cert_pem);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    const size_t len = (size_t)(favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, favicon_ico_start, len);
}

/* ---- PWA static assets (manifest, service worker, icons) ----------------- */

static esp_err_t manifest_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/manifest+json");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, manifest_start, (size_t)(manifest_end - manifest_start));
}

static esp_err_t sw_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    /* The worker must not be cached long or an update never reaches an installed
     * client; let the browser revalidate it every load. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, sw_js_start, (size_t)(sw_js_end - sw_js_start));
}

static esp_err_t icon192_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, icon_192_start, (size_t)(icon_192_end - icon_192_start));
}

static esp_err_t icon512_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, icon_512_start, (size_t)(icon_512_end - icon_512_start));
}

/* True when this request arrived on the rescue hotspot's interface: the softAP is
 * always the device's own 192.168.4.1, so a connection whose local address is that
 * came in over the AP, not the station/Ethernet link. Used to tell captive-portal
 * clients apart from ordinary ones on the shared port-80 server. */
static bool req_on_ap_iface(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
        return false;
    }
    /* The softAP always hands the device 192.168.4.1, so a socket whose local
     * address is exactly that came in over the AP, not the station/Ethernet link.
     * Compared numerically (covers the IPv4-mapped ::ffff:192.168.4.1 form too)
     * rather than by substring, which would also match e.g. 192.168.4.1x. */
    const uint32_t ap_addr = htonl(0xC0A80401u); /* 192.168.4.1, network byte order */
    if (ss.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&ss)->sin_addr.s_addr == ap_addr;
    }
    if (ss.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        uint32_t v4;
        memcpy(&v4, b + 12, sizeof(v4));
        return memcmp(b, v4mapped, sizeof(v4mapped)) == 0 && v4 == ap_addr;
    }
    return false;
}

/* The hotel-style landing page the OS "sign in to network" sheet shows when a
 * device joins the hotspot. Served in the clear (the captive mini-browser chokes
 * on the self-signed TLS cert). Its button opens the console: in AP mode the
 * console is served plain over this same origin, so the button is a same-origin
 * link that works right inside the captive sheet; in a TLS setup it points at the
 * https console, which the full browser handles. */
static esp_err_t captive_landing(httpd_req_t *req)
{
    const char *hn = kvm_setting_str("net_hostname");
    if (!hn || !hn[0]) {
        hn = CONFIG_KVM_MDNS_HOSTNAME;
    }
    /* In AP mode the console is plain HTTP on this port (below); otherwise it is
     * only reachable over TLS and the button must use https + the cert-matching
     * hostname. */
    const bool ap_mode = (kvm_setting_int("net_mode") == KVM_NET_WIFI_AP) ||
                         kvm_wifi_setup_ap_active();
    const bool plain = ap_mode || !kvm_setting_bool("sec_https");
    char console_url[64];
    if (plain) {
        /* Same origin. The captive sheet's address is a hijacked probe host, but
         * every name resolves to us, so "/" lands on the console here. */
        snprintf(console_url, sizeof(console_url), "/");
    } else {
        snprintf(console_url, sizeof(console_url), "https://%s.local/", hn);
    }
    char body[1400];
    int n = snprintf(
        body, sizeof(body),
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>ESP-KVM</title>"
        "<style>body{margin:0;min-height:100vh;display:flex;align-items:center;"
        "justify-content:center;background:#0b1020;color:#e8ecf5;"
        "font:16px/1.5 system-ui,sans-serif}.c{text-align:center;padding:24px}"
        "h1{font-size:22px;margin:.2em 0}p{color:#9aa4bf;margin:.4em 0}"
        "a.b{display:inline-block;margin-top:18px;padding:12px 22px;border-radius:10px;"
        "background:#3b82f6;color:#fff;text-decoration:none;font-weight:600}"
        "a.s{display:block;margin-top:14px;color:#6b7699;font-size:13px}"
        "p.n{margin-top:18px;font-size:13px;max-width:22em}</style>"
        "<div class=c>"
        "<h1>ESP-KVM</h1><p>Setup hotspot active.</p>"
        "<a class=b href=\"%s\">Open console &rarr;</a>"
        "<p class=n>This sheet is not a browser and handles the console poorly. "
        "For anything more than a look, open <b>http://192.168.4.1/</b> in your "
        "browser - and keep this network when the phone warns it has no "
        "internet, or it will send the request out the wrong way.</p>"
        "<a class=s href=\"/cert.pem\">Download CA certificate</a></div>",
        console_url);
    if (n <= 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "portal");
    }
    /* 200 with unexpected content is what makes iOS/Android/Windows treat the
     * network as captive and pop the sheet showing this page. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/* 404 handler for AP mode: the console is served plain on this port, so any path
 * the OS connectivity check probes (which does not match a real route) gets the
 * captive landing, popping the "sign in" sheet. Real routes (/, /api, ...) are
 * registered and never reach here. */
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return captive_landing(req);
}

/*
 * Everything that arrives on port 80 while TLS is on is answered with a
 * redirect and nothing else. No URI handler is registered on that server, so
 * every request lands in the 404 handler - which is exactly the "any request"
 * hook this needs.
 */
static esp_err_t redirect_to_https(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    /* On the rescue hotspot, don't bounce to HTTPS (the captive sheet can't clear
     * the self-signed cert warning) - show the landing page instead. */
    if (req_on_ap_iface(req)) {
        return captive_landing(req);
    }
    char host[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !host[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no Host header");
    }
    /* The Host header carries the port this server answers on; the browser must
     * be sent to the TLS one. */
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }

    char location[160];
    const char *uri = req->uri[0] ? req->uri : "/";
    int n = snprintf(location, sizeof(location), "https://%s%s", host, uri);
    if (n <= 0 || n >= (int)sizeof(location)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request too long to redirect");
    }
    httpd_resp_set_status(req, "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

/*
 * Register one route, and say so when it does not fit.
 *
 * esp_http_server returns ESP_ERR_HTTPD_HANDLERS_FULL and moves on; the route
 * then simply does not exist, with nothing in the log to say why. That silence
 * has cost this project two debugging sessions - once the video WebSocket, once
 * the input one - so it ends here.
 */
/*
 * Every ordinary route is answered through here, so the security headers are
 * set once for all of them. The real handler travels in user_ctx, which no
 * route of ours uses for anything else; a WebSocket keeps its own handler,
 * because by the time it runs the handshake response is long gone.
 */
static esp_err_t route_with_headers(httpd_req_t *req)
{
    kvm_web_security_headers(req);
    esp_err_t (*const handler)(httpd_req_t *) = (esp_err_t (*)(httpd_req_t *))req->user_ctx;
    return handler(req);
}

/*
 * The server answers "no such page" and "wrong method" from inside itself, so
 * those replies never pass through a route of ours and used to go out with no
 * headers at all. Same body and the same closed socket as before - the browser
 * is simply told how to treat the page, like every other answer.
 */
static esp_err_t err_with_headers(httpd_req_t *req, httpd_err_code_t error)
{
    kvm_web_security_headers(req);
    (void)httpd_resp_send_err(req, error, NULL);
    /* What the default does when no handler is registered. */
    return ESP_FAIL;
}

static void register_err_handlers(httpd_handle_t h)
{
    for (int code = 0; code < HTTPD_ERR_CODE_MAX; code++) {
        const esp_err_t err = httpd_register_err_handler(h, (httpd_err_code_t)code, err_with_headers);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "error handler %d not registered: %s", code, esp_err_to_name(err));
        }
    }
}

static void register_route(httpd_handle_t h, const httpd_uri_t *u)
{
    httpd_uri_t wrapped = *u;
    if (!u->is_websocket && !u->user_ctx) {
        wrapped.handler = route_with_headers;
        wrapped.user_ctx = (void *)u->handler;
    }
    const esp_err_t err = httpd_register_uri_handler(h, &wrapped);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route %s NOT registered: %s - raise max_uri_handlers", u->uri,
                 esp_err_to_name(err));
    }
}

static httpd_handle_t start_redirect_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32769; /* the TLS server owns the default */
    cfg.max_uri_handlers = 2;
    /* This server also serves the captive-portal landing page, and a phone doing
     * captive detection opens several short connections at once; too few sockets
     * makes accept() fail, which makes the OS keep retrying. Purging reclaims the
     * short-lived ones. (Budget: 7 TLS + 6 here + listeners stays within
     * LWIP_MAX_ACTIVE_TCP=16.) */
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    cfg.max_req_hdr_len = 1024;

    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no redirect server on port 80");
        return NULL;
    }
    /* The certificate download is served here too, in the clear, so it can be
     * fetched and trusted without first clicking through the warning it fixes.
     * Everything else on port 80 redirects to https. */
    httpd_uri_t u_cert = {.uri = "/cert.pem", .method = HTTP_GET, .handler = cert_get};
    register_route(h, &u_cert);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, redirect_to_https);
    return h;
}

httpd_handle_t http_server_start(void)
{
    if (!ws_server_init()) {
        return NULL;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Browsers send >1 KiB of headers; Vite dev proxy forwards them. Default 1024 -> 431. */
    cfg.max_req_hdr_len = 8192;
    cfg.server_port = 80;
    /* Peak 7.2 KB over hours of use with TLS (2026-10-10); certificates are
     * made on a task of their own. Internal RAM is what runs out. */
    cfg.stack_size = 12 * 1024;
    /* Prefer draining TCP slightly above capture so multipart frames reach the browser. */
    cfg.task_priority = tskIDLE_PRIORITY + 6;
    cfg.send_wait_timeout = 30;
    /* One task serves every session, so this is how long one peer that stops
     * mid-request keeps everyone else waiting (#25). Uploads survive a short
     * value now: the body reads wait out a silence, see http_recv.h. */
    cfg.recv_wait_timeout = 5;
    cfg.keep_alive_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = http_sess_open_cb;
    cfg.close_fn = http_sess_close_cb;
    /*
     * Sessions must be reclaimable. Upstream disabled LRU purging to protect the
     * long-lived /stream from being evicted, but the server does not free a
     * session whose peer vanished without a clean close - it sees ENOTCONN and
     * keeps it. With purging off those sessions accumulate until the pool is
     * exhausted, and the device stays pingable with a dead web interface after
     * roughly twenty page loads. Purging costs at worst a reconnect of the
     * oldest viewer; not purging costs the whole interface, permanently.
     */
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 12;
    /*
     * Must exceed the total number of registered handlers, and this has now bitten
     * twice. Registration past the limit fails silently, and /video and /ws are
     * registered last, so an overflow drops exactly the WebSocket endpoints: video
     * and input die with a 404 on wss while every REST route still answers, which
     * looks like anything but a table that is one entry too small.
     *
     * The count on 2026-10-25 is about 104 (count the `{.uri = ` rows across this
     * component); the profiles' five took it there. Adding the log endpoint once
     * took it from 44 to 45 and cost the keyboard; the recorder's routes took it
     * past 64.
     *
     * The check below now logs a failed registration rather than swallowing it, so
     * the next person gets a line instead of a mystery - but keep headroom anyway.
     */
    cfg.max_uri_handlers = 144;

    if (kvm_auth_init() != ESP_OK) {
        /* Without a working password store the only safe answer is not to
         * serve at all: a KVM that accepts every password is worse than one
         * that is unreachable. */
        ESP_LOGE(TAG, "authentication could not be initialised; refusing to start the server");
        return NULL;
    }

    httpd_handle_t h = NULL;
    /*
     * In AP (setup hotspot) mode the console is served in the clear, even if TLS
     * is otherwise enabled: a phone's captive-portal browser cannot get past the
     * self-signed certificate, so a plain console on 192.168.4.1 is the only thing
     * that actually works there - the same tradeoff every router's setup page
     * makes. H.264 (which needs a secure context) is unavailable over the hotspot;
     * MJPEG and all of settings still work, which is what a rescue/setup link is for.
     */
    const bool ap_mode = (kvm_setting_int("net_mode") == KVM_NET_WIFI_AP) ||
                         kvm_wifi_setup_ap_active();
    /*
     * TLS is the default because this device is a way into another machine, and
     * because the console needs a secure page for WebCodecs - without it the
     * H.264 decoder does not exist in any browser. It can be switched off for a
     * trusted network, where the memory a TLS session costs is better spent on
     * frames.
     */
    if (kvm_setting_bool("sec_https") && !ap_mode) {
        /*
         * Try the operator's own certificate first (if one is installed); if the
         * TLS stack refuses it, fall back to the self-signed identity rather than
         * leave the console unreachable over a bad upload. At most two attempts:
         * bring-your-own, then self-signed.
         */
        kvm_acme_init();
        bool allow_byo = true;
        for (int attempt = 0; attempt < 2; attempt++) {
            kvm_tls_identity_t id;
            esp_err_t cert_err = kvm_tls_identity_get(&id, allow_byo);
            if (cert_err != ESP_OK) {
                kvm_cap_report(KVM_CAP_HTTPS, false, "no certificate could be generated (%s)",
                               esp_err_to_name(cert_err));
                break;
            }
            /* Keep the CA certificate around so a client can download and trust
             * it; the leaf and the private key are never offered for download.
             * An operator certificate has no CA to import (id.ca_pem is NULL). */
            free(s_cert_pem);
            s_cert_pem = id.ca_pem ? strdup(id.ca_pem) : NULL;

            httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
            ssl.httpd = cfg;
            ssl.port_secure = 443;
            /* Each open TLS session holds buffers even with dynamic allocation
             * on, so fewer of them than plain HTTP allows. */
            ssl.httpd.max_open_sockets = 7;
            ssl.servercert = (const unsigned char *)id.cert_pem;
            ssl.servercert_len = id.cert_len;
            ssl.prvtkey_pem = (const unsigned char *)id.key_pem;
            ssl.prvtkey_len = id.key_len;
            ssl.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
            /*
             * A full handshake costs this chip about a third of a second, and
             * the console opens several connections - the page, the video
             * channel, the control channel. Resumption turns all but the first
             * into a few milliseconds.
             */
            ssl.session_tickets = true;
            /* The Let's Encrypt certificate, for clients asking for the tailnet name. */
            ssl.cert_select_cb = kvm_acme_sni_cb;

            const bool was_byo = id.byo;
            esp_err_t err = httpd_ssl_start(&h, &ssl);
            /* esp_https_server copies the PEMs into its own configuration. */
            kvm_tls_identity_free(&id);
            if (err == ESP_OK) {
                kvm_cap_report(KVM_CAP_HTTPS, true, NULL);
                s_redirect_httpd = start_redirect_server();
                ESP_LOGI(TAG, "serving on https://, port 80 redirects%s",
                         was_byo ? " (operator certificate)" : "");
                break;
            }
            ESP_LOGE(TAG, "httpd_ssl_start: %s", esp_err_to_name(err));
            h = NULL;
            if (was_byo) {
                ESP_LOGE(TAG, "the operator certificate was rejected; falling back to self-signed");
                allow_byo = false;
                continue; /* retry with the self-signed identity */
            }
            kvm_cap_report(KVM_CAP_HTTPS, false, "the TLS server did not start (%s)",
                           esp_err_to_name(err));
            break;
        }
    } else {
        kvm_cap_report(KVM_CAP_HTTPS, true, NULL);
    }

    if (!h && httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return NULL;
    }
    s_httpd = h;
    video_ws_init(h);
    usb_hid_set_led_callback(on_hid_leds, NULL);

    register_err_handlers(h);

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    register_route(h, &u_root);
    httpd_uri_t u_favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    register_route(h, &u_favicon);
    httpd_uri_t u_cert = {.uri = "/cert.pem", .method = HTTP_GET, .handler = cert_get};
    register_route(h, &u_cert);
    /* PWA: manifest, service worker and icons (unauthenticated static assets). */
    httpd_uri_t u_manifest = {
        .uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = manifest_get};
    register_route(h, &u_manifest);
    httpd_uri_t u_sw = {.uri = "/sw.js", .method = HTTP_GET, .handler = sw_get};
    register_route(h, &u_sw);
    httpd_uri_t u_icon192 = {.uri = "/icon-192.png", .method = HTTP_GET, .handler = icon192_get};
    register_route(h, &u_icon192);
    httpd_uri_t u_icon512 = {.uri = "/icon-512.png", .method = HTTP_GET, .handler = icon512_get};
    register_route(h, &u_icon512);
    httpd_uri_t u_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    register_route(h, &u_stream);
    static const httpd_uri_t api_uris[] = {
        {.uri = "/api/capabilities", .method = HTTP_GET, .handler = api_capabilities_get},
        {.uri = "/api/v1/settings/schema", .method = HTTP_GET, .handler = api_settings_schema_get},
        {.uri = "/api/v1/pins", .method = HTTP_GET, .handler = api_pins_get},
        {.uri = "/api/v1/video/status", .method = HTTP_GET, .handler = api_video_status_get},
        {.uri = "/api/v1/viewers", .method = HTTP_GET, .handler = api_viewers_get},
        {.uri = "/api/v1/system/info", .method = HTTP_GET, .handler = api_system_info_get},
        {.uri = "/api/v1/system/usbprobe", .method = HTTP_GET, .handler = api_system_usbprobe_get},
        {.uri = "/api/v1/wifi/scan", .method = HTTP_POST, .handler = api_wifi_scan_post},
        {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_get},
        {.uri = "/api/v1/wifi/coproc/update",
         .method = HTTP_POST,
         .handler = api_wifi_coproc_update_post},
        {.uri = "/api/v1/system/update", .method = HTTP_POST, .handler = api_system_update_post},
        {.uri = "/api/v1/system/update/keep", .method = HTTP_POST, .handler = api_system_update_keep_post},
        {.uri = "/api/v1/system/update/rollback", .method = HTTP_POST, .handler = api_system_update_rollback_post},
        {.uri = "/api/v1/system/log", .method = HTTP_GET, .handler = api_system_log_get},
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        {.uri = "/api/v1/system/coredump", .method = HTTP_GET, .handler = api_system_coredump_get},
        {.uri = "/api/v1/system/coredump",
         .method = HTTP_DELETE,
         .handler = api_system_coredump_delete},
#endif
        {.uri = "/api/v1/settings/reset", .method = HTTP_POST, .handler = api_settings_reset_post},
        {.uri = "/api/v1/system/restart", .method = HTTP_POST, .handler = api_system_restart_post},
        {.uri = "/api/v1/system/boot-slot", .method = HTTP_POST, .handler = api_system_boot_slot_post},
        {.uri = "/api/v1/system/install", .method = HTTP_POST, .handler = api_system_install_post},
        {.uri = "/api/v1/system/install", .method = HTTP_GET, .handler = api_system_install_get},
        {.uri = "/api/v1/tls", .method = HTTP_GET, .handler = api_tls_get},
        {.uri = "/api/v1/auth/token", .method = HTTP_POST, .handler = api_token_post},
        {.uri = "/api/v1/auth/token", .method = HTTP_DELETE, .handler = api_token_delete},
        {.uri = "/api/v1/tls/cert", .method = HTTP_PUT, .handler = api_tls_cert_put},
        {.uri = "/api/v1/tls/cert", .method = HTTP_DELETE, .handler = api_tls_cert_delete},
        {.uri = "/api/v1/tls/acme", .method = HTTP_POST, .handler = api_tls_acme_post},
        {.uri = "/api/v1/power/wake", .method = HTTP_POST, .handler = api_power_wake_post},
        {.uri = "/api/v1/power/usb", .method = HTTP_POST, .handler = api_power_usb_post},
        {.uri = "/api/v1/power/click", .method = HTTP_POST, .handler = api_power_click_post},
        {.uri = "/api/v1/power/hold", .method = HTTP_POST, .handler = api_power_hold_post},
        {.uri = "/api/v1/power/reset", .method = HTTP_POST, .handler = api_power_reset_post},
        {.uri = "/api/v1/settings", .method = HTTP_GET, .handler = api_settings_get},
        {.uri = "/api/v1/settings", .method = HTTP_PUT, .handler = api_settings_put},
        {.uri = "/api/v1/storage/images", .method = HTTP_GET, .handler = api_storage_images_get},
        {.uri = "/api/v1/storage/upload", .method = HTTP_POST, .handler = api_storage_upload_post},
        {.uri = "/api/v1/storage/delete", .method = HTTP_POST, .handler = api_storage_delete_post},
        {.uri = "/api/v1/storage/rescue", .method = HTTP_POST, .handler = api_storage_rescue_post},
        /* Agent / CUA surface: a still frame and HID primitives over plain REST. */
        {.uri = "/api/v1/video/frame.jpg", .method = HTTP_GET, .handler = api_video_frame_get},
        {.uri = "/api/v1/screen/text", .method = HTTP_GET, .handler = api_screen_text_get},
        {.uri = "/api/v1/hid/move", .method = HTTP_POST, .handler = api_hid_move_post},
        {.uri = "/api/v1/hid/click", .method = HTTP_POST, .handler = api_hid_click_post},
        {.uri = "/api/v1/hid/key", .method = HTTP_POST, .handler = api_hid_key_post},
        {.uri = "/api/v1/hid/type", .method = HTTP_POST, .handler = api_hid_type_post},
        {.uri = "/api/v1/hid/pad", .method = HTTP_POST, .handler = api_hid_pad_post},
        {.uri = "/api/v1/hid/reattach", .method = HTTP_POST, .handler = api_hid_reattach_post},
        {.uri = "/api/v1/runbooks/status", .method = HTTP_GET, .handler = api_runbooks_status_get},
        {.uri = "/api/v1/runbooks/run", .method = HTTP_POST, .handler = api_runbooks_run_post},
        {.uri = "/api/v1/runbooks/stop", .method = HTTP_POST, .handler = api_runbooks_stop_post},
        {.uri = "/api/v1/video/reconnect", .method = HTTP_POST, .handler = api_video_reconnect_post},
        {.uri = "/api/v1/schedules/status", .method = HTTP_GET, .handler = api_schedules_status_get},
        {.uri = "/api/v1/schedules/run", .method = HTTP_POST, .handler = api_schedules_run_post},
        {.uri = "/api/v1/notify/status", .method = HTTP_GET, .handler = api_notify_status_get},
        {.uri = "/api/v1/notify/test", .method = HTTP_POST, .handler = api_notify_test_post},
        {.uri = "/api/v1/notify/chats", .method = HTTP_POST, .handler = api_notify_chats_post},
    };
    for (size_t i = 0; i < sizeof(api_uris) / sizeof(api_uris[0]); i++) {
        register_route(h, &api_uris[i]);
    }
    size_t n_record = 0;
    const httpd_uri_t *record_uris = record_api_routes(&n_record);
    for (size_t i = 0; i < n_record; i++) {
        register_route(h, &record_uris[i]);
    }
    size_t n_cec = 0;
    const httpd_uri_t *cec_uris = cec_api_routes(&n_cec);
    for (size_t i = 0; i < n_cec; i++) {
        register_route(h, &cec_uris[i]);
    }
    size_t n_ts = 0;
    const httpd_uri_t *ts_uris = ts_api_routes(&n_ts);
    for (size_t i = 0; i < n_ts; i++) {
        register_route(h, &ts_uris[i]);
    }
    size_t n_push = 0;
    const httpd_uri_t *push_uris = push_api_routes(&n_push);
    for (size_t i = 0; i < n_push; i++) {
        register_route(h, &push_uris[i]);
    }
    size_t n_prof = 0;
    const httpd_uri_t *prof_uris = profiles_api_routes(&n_prof);
    for (size_t i = 0; i < n_prof; i++) {
        register_route(h, &prof_uris[i]);
    }
    size_t n_rgn = 0;
    const httpd_uri_t *rgn_uris = regions_api_routes(&n_rgn);
    for (size_t i = 0; i < n_rgn; i++) {
        register_route(h, &rgn_uris[i]);
    }
    size_t n_fetch = 0;
    const httpd_uri_t *fetch_uris = url_fetch_routes(&n_fetch);
    for (size_t i = 0; i < n_fetch; i++) {
        register_route(h, &fetch_uris[i]);
    }
    size_t n_netlog = 0;
    const httpd_uri_t *netlog_uris = netlog_api_routes(&n_netlog);
    for (size_t i = 0; i < n_netlog; i++) {
        register_route(h, &netlog_uris[i]);
    }
    size_t n_probe = 0;
    const httpd_uri_t *probe_uris = pinprobe_api_routes(&n_probe);
    for (size_t i = 0; i < n_probe; i++) {
        register_route(h, &probe_uris[i]);
    }
    size_t n_webrtc = 0;
    const httpd_uri_t *webrtc_uris = webrtc_api_routes(&n_webrtc);
    for (size_t i = 0; i < n_webrtc; i++) {
        register_route(h, &webrtc_uris[i]);
    }
    size_t n_serial = 0;
    const httpd_uri_t *serial_uris = serial_api_routes(&n_serial);
    for (size_t i = 0; i < n_serial; i++) {
        register_route(h, &serial_uris[i]);
    }
    size_t n_tools = 0;
    const httpd_uri_t *tools_uris = tools_api_routes(&n_tools);
    for (size_t i = 0; i < n_tools; i++) {
        register_route(h, &tools_uris[i]);
    }
    if (audio_api_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio socket: no memory for its pump");
    }
    size_t n_audio = 0;
    const httpd_uri_t *audio_uris = audio_api_routes(&n_audio);
    for (size_t i = 0; i < n_audio; i++) {
        register_route(h, &audio_uris[i]);
    }

    /* Before anything else is registered: a request that arrives while the
     * password store is still loading must not be served. */
    kvm_auth_register(h);

    httpd_uri_t u_video = {
        .uri = "/video",
        .method = HTTP_GET,
        .handler = video_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
    };
    register_route(h, &u_video);

    httpd_uri_t u_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_input_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
    };
    register_route(h, &u_ws);

    /* AP mode: turn unmatched requests (the OS connectivity probe) into the
     * captive landing so the "sign in to network" sheet pops on join. The console
     * itself is on registered routes and is untouched. */
    if (ap_mode) {
        httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, captive_404);
    }
    return h;
}
