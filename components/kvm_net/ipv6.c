/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_ipv6.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "kvm_settings.h"

static const char *TAG = "net6";

#if CONFIG_LWIP_IPV6

#include "lwip/sockets.h" /* inet_ntop, for the compressed textual form */

static esp_netif_t *s_netif;
static kvm_ipv6_identity_cb_t s_identity_cb;
/** True while the worker below is writing to NVS, so several addresses arriving
 *  in a row do not stack up a task each. The callback itself is a no-op when
 *  nothing changed, so re-running it as addresses appear costs nothing. */
static volatile bool s_identity_busy;

/**
 * How reachable an address is, so the most useful one is reported first. A
 * global address is what a browser elsewhere can use; a unique-local one works
 * inside the site; a link-local one only on the same wire, and only with a zone
 * index the browser will not supply.
 */
static int addr_rank(const esp_ip6_addr_t *addr)
{
    switch (esp_netif_ip6_get_addr_type(addr)) {
    case ESP_IP6_ADDR_IS_GLOBAL:
        return 3;
    case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:
        return 2;
    case ESP_IP6_ADDR_IS_SITE_LOCAL:
        return 1;
    default:
        return 0; /* link-local, or an address the stack has not classified */
    }
}

static void addr_str(const esp_ip6_addr_t *addr, char *out, size_t len)
{
    out[0] = '\0';
    inet_ntop(AF_INET6, addr->addr, out, (socklen_t)len);
}

static bool ipv6_enabled(void)
{
    return kvm_setting_bool("net_ipv6");
}

/**
 * Collect the interface's addresses, sorted so the most routable comes first.
 * Returns how many were found. The lists are tiny (three entries at most), so
 * an insertion sort is the whole of it.
 */
static int collect(esp_ip6_addr_t *addrs, int max)
{
    if (!s_netif || !ipv6_enabled()) {
        return 0;
    }
    esp_ip6_addr_t all[KVM_IP6_MAX_ADDRS];
    int n = esp_netif_get_all_ip6(s_netif, all);
    if (n < 0) {
        return 0;
    }
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        int j = i;
        addrs[i] = all[i];
        while (j > 0 && addr_rank(&addrs[j]) > addr_rank(&addrs[j - 1])) {
            const esp_ip6_addr_t tmp = addrs[j - 1];
            addrs[j - 1] = addrs[j];
            addrs[j] = tmp;
            j--;
        }
    }
    return n;
}

int kvm_ipv6_addrs(char out[][KVM_IP6_STRLEN], int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    esp_ip6_addr_t addrs[KVM_IP6_MAX_ADDRS];
    const int n = collect(addrs, max < KVM_IP6_MAX_ADDRS ? max : KVM_IP6_MAX_ADDRS);
    for (int i = 0; i < n; i++) {
        addr_str(&addrs[i], out[i], KVM_IP6_STRLEN);
    }
    return n;
}

bool kvm_ipv6_routable(char *out, size_t len)
{
    if (out && len) {
        out[0] = '\0';
    }
    esp_ip6_addr_t addrs[KVM_IP6_MAX_ADDRS];
    const int n = collect(addrs, KVM_IP6_MAX_ADDRS);
    if (n == 0 || addr_rank(&addrs[0]) == 0) {
        return false; /* nothing, or only a link-local address */
    }
    if (out && len) {
        addr_str(&addrs[0], out, len);
    }
    return true;
}

/*
 * The two addresses worth naming in the certificate. A global address is what
 * reaches the device from outside; a unique-local one is what survives the ISP
 * handing out a different prefix, which is the address someone who bookmarks the
 * console actually wants. Either may come back empty.
 */
static void best_pair(char *global, char *ula, size_t len)
{
    global[0] = '\0';
    ula[0] = '\0';
    esp_ip6_addr_t addrs[KVM_IP6_MAX_ADDRS];
    const int n = collect(addrs, KVM_IP6_MAX_ADDRS);
    for (int i = 0; i < n; i++) {
        /* Sorted most-routable-first, so the first of each kind is the best. */
        const esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&addrs[i]);
        if (type == ESP_IP6_ADDR_IS_GLOBAL && !global[0]) {
            addr_str(&addrs[i], global, len);
        } else if (type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL && !ula[0]) {
            addr_str(&addrs[i], ula, len);
        }
    }
}

typedef struct {
    char global[KVM_IP6_STRLEN];
    char ula[KVM_IP6_STRLEN];
} ip6_identity_t;

/*
 * Handing the addresses to the TLS layer writes them to NVS, which wants more
 * stack than the system event task has (2.3 KB). So it happens on a task made
 * for it and thrown away afterwards, the way certificate generation does.
 */
static void identity_task(void *arg)
{
    ip6_identity_t *id = (ip6_identity_t *)arg;
    if (s_identity_cb && s_identity_cb(id->global, id->ula)) {
        ESP_LOGI(TAG, "recorded for the certificate; it names these addresses from "
                      "the next restart");
    }
    free(id);
    s_identity_busy = false;
    vTaskDelete(NULL);
}

/*
 * A new address became usable. Autoconfiguration produces them one at a time and
 * a link-local one always arrives first, so this fires two or three times on a
 * dual-stack network rather than once.
 */
static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip6_t *e = (ip_event_got_ip6_t *)data;
    if (!e || (s_netif && e->esp_netif != s_netif)) {
        return;
    }
    char text[KVM_IP6_STRLEN];
    addr_str(&e->ip6_info.ip, text, sizeof(text));
    const int rank = addr_rank(&e->ip6_info.ip);
    if (rank == 0) {
        /* Worth a line - it is the first sign IPv6 came up at all - but not an
         * address anyone can open, so it is not offered as one. */
        ESP_LOGI(TAG, "IPv6 link-local address %s", text);
        return;
    }
    const char *scheme = kvm_setting_bool("sec_https") ? "https" : "http";
    ESP_LOGI(TAG, "Got IPv6: %s - open %s://[%s]/", text, scheme, text);

    /*
     * Name them in the certificate. Both kinds, not just the one that arrived:
     * addresses come in one at a time and the global one usually lands first, so
     * naming only the newest would leave the unique-local address - the stable
     * one, and so the one people bookmark - out of the certificate for good.
     *
     * Unlike the tailnet address this does not restart the device to re-issue the
     * leaf straight away: a routable address appears seconds into every boot on
     * an IPv6 network, and a KVM that reboots itself just after coming up is
     * worse than a certificate that names the address from the next restart. The
     * mDNS name in the same certificate already covers the usual way in.
     */
    if (s_identity_cb && !s_identity_busy) {
        ip6_identity_t *id = calloc(1, sizeof(*id));
        if (id) {
            best_pair(id->global, id->ula, KVM_IP6_STRLEN);
            s_identity_busy = true;
            if (xTaskCreate(identity_task, "ip6_ident", 4096, id, 4, NULL) != pdPASS) {
                s_identity_busy = false;
                free(id); /* the next address to arrive tries again */
            }
        }
    }
}

void kvm_ipv6_set_identity_cb(kvm_ipv6_identity_cb_t cb)
{
    s_identity_cb = cb;
}

void kvm_ipv6_start(esp_netif_t *netif)
{
    if (!netif) {
        return;
    }
    if (!ipv6_enabled()) {
        ESP_LOGI(TAG, "IPv6 is off (net_ipv6)");
        return;
    }
    /* Link-up can be reported more than once (a cable replugged, a WiFi
     * reassociation), and neither the handler nor the address needs redoing. */
    if (s_netif == netif) {
        return;
    }
    s_netif = netif;
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL);
    /* The link-local address is ours to form; everything routable comes from the
     * router's advertisements. Without a link-local address there is no way to
     * talk to the router, so this has to happen even on a network with no v6. */
    const esp_err_t err = esp_netif_create_ip6_linklocal(netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IPv6 link-local address: %s", esp_err_to_name(err));
    }
}

#else /* !CONFIG_LWIP_IPV6 */

void kvm_ipv6_set_identity_cb(kvm_ipv6_identity_cb_t cb)
{
    (void)cb;
}

void kvm_ipv6_start(esp_netif_t *netif)
{
    (void)netif;
    ESP_LOGW(TAG, "IPv6 is not compiled in (CONFIG_LWIP_IPV6)");
}

int kvm_ipv6_addrs(char out[][KVM_IP6_STRLEN], int max)
{
    (void)out;
    (void)max;
    return 0;
}

bool kvm_ipv6_routable(char *out, size_t len)
{
    if (out && len) {
        out[0] = '\0';
    }
    return false;
}

#endif /* CONFIG_LWIP_IPV6 */
