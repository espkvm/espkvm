/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ethernet.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "kvm_settings.h"

static const char *TAG = "net";

esp_err_t kvm_wol_send(const char *mac)
{
    /* Parse the six MAC octets. sscanf with %hhx keeps this to one line and
     * rejects anything that is not six colon-separated hex bytes. */
    uint8_t m[6];
    if (!mac || sscanf(mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &m[0], &m[1], &m[2], &m[3], &m[4],
                       &m[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The magic packet: six 0xFF bytes, then the target MAC sixteen times. */
    uint8_t pkt[6 + 16 * 6];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, m, 6);
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return ESP_FAIL;
    }
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(9), /* discard port, the usual WoL destination */
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    int sent = sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(s);
    if (sent != (int)sizeof(pkt)) {
        ESP_LOGW(TAG, "WoL send failed (%d)", sent);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WoL magic packet sent to %s", mac);
    return ESP_OK;
}

#if CONFIG_KVM_ETH_ENABLE
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;

static void eth_on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    /* The scheme the server is actually listening on: printing http:// while
     * port 80 only redirects sends the reader through an extra hop. */
    const char *scheme = kvm_setting_bool("sec_https") ? "https" : "http";
    ESP_LOGI(TAG, "Open %s://" IPSTR "/ or %s://%s.local/", scheme, IP2STR(&e->ip_info.ip), scheme,
             kvm_setting_str("net_hostname"));
}

/* Live link state, so the console can show whether the cable is up and at what
 * speed. Updated from the driver's connect/disconnect events. */
static volatile bool s_eth_up = false;
static volatile int s_eth_mbps = 0;

static void eth_on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
            s_eth_mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
        }
        s_eth_up = true;
        ESP_LOGI(TAG, "Ethernet link up (%d Mbps)", s_eth_mbps);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_eth_up = false;
        s_eth_mbps = 0;
        ESP_LOGI(TAG, "Ethernet link down");
    }
}

void kvm_eth_link(bool *up, int *mbps)
{
    if (up) {
        *up = s_eth_up;
    }
    if (mbps) {
        *mbps = s_eth_mbps;
    }
}
#endif /* CONFIG_KVM_ETH_ENABLE */

void kvm_net_advertise(const char *hostname)
{
    esp_err_t mdns_err = mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
        return;
    }
    mdns_err = mdns_hostname_set(hostname);
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname: %s", esp_err_to_name(mdns_err));
    }
    mdns_err = mdns_instance_name_set("ESP-KVM");
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance: %s", esp_err_to_name(mdns_err));
    }
    const bool tls = kvm_setting_bool("sec_https");
    mdns_txt_item_t http_txt[] = {
        {"path", "/"},
    };
    /* Advertised under the service the device really answers, so a browser or a
     * discovery tool lands on the working port. */
    mdns_err = mdns_service_add("ESP-KVM", tls ? "_https" : "_http", "_tcp", tls ? 443 : 80,
                                http_txt, 1);
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service: %s", esp_err_to_name(mdns_err));
    } else {
        ESP_LOGI(TAG, "mDNS: %s://%s.local/", tls ? "https" : "http", hostname);
    }
}

#if CONFIG_KVM_ETH_ENABLE

esp_err_t ethernet_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_KVM_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_KVM_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = CONFIG_KVM_ETH_RMII_CLK_GPIO;
#if SOC_EMAC_USE_MULTI_IO_MUX
    emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_KVM_ETH_RMII_TX_EN_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_KVM_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_KVM_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_KVM_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_KVM_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_KVM_ETH_RMII_RXD1_GPIO;
#endif
    emac_config.smi_gpio.mdc_num = CONFIG_KVM_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_KVM_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "eth mac");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "eth phy");
        return ESP_FAIL;
    }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "eth install");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_FAIL, TAG, "netif");
    /* The name is a setting, not a build-time constant: two of these on one
     * network otherwise answer to the same mDNS address. */
    const char *hostname = kvm_setting_str("net_hostname");
    if (!hostname || !hostname[0]) {
        hostname = CONFIG_KVM_MDNS_HOSTNAME;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_eth_netif, hostname), TAG, "hostname");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "glue");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_on_got_ip, NULL),
                        TAG, "ip ev");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_on_event, NULL), TAG, "eth ev");

    /*
     * Static addressing, when the operator has turned DHCP off. A malformed
     * address falls back to DHCP rather than stranding the device on boot; a
     * valid-but-wrong one (unreachable gateway, say) is recovered with the reset
     * button, which reverts to DHCP the same way it clears a forgotten password.
     */
    if (!kvm_setting_bool("net_dhcp")) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_str_to_ip4(kvm_setting_str("net_ip"), &ip.ip) == ESP_OK &&
            esp_netif_str_to_ip4(kvm_setting_str("net_mask"), &ip.netmask) == ESP_OK &&
            esp_netif_str_to_ip4(kvm_setting_str("net_gw"), &ip.gw) == ESP_OK) {
            esp_netif_dhcpc_stop(s_eth_netif); /* ok if it was not running yet */
            if (esp_netif_set_ip_info(s_eth_netif, &ip) == ESP_OK) {
                esp_netif_dns_info_t dns = {0};
                if (esp_netif_str_to_ip4(kvm_setting_str("net_dns"), &dns.ip.u_addr.ip4) == ESP_OK) {
                    dns.ip.type = ESP_IPADDR_TYPE_V4;
                    esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
                }
                const char *scheme = kvm_setting_bool("sec_https") ? "https" : "http";
                ESP_LOGI(TAG, "Static IP. Open %s://" IPSTR "/ or %s://%s.local/", scheme,
                         IP2STR(&ip.ip), scheme, hostname);
            } else {
                ESP_LOGW(TAG, "could not apply the static address; falling back to DHCP");
                esp_netif_dhcpc_start(s_eth_netif);
            }
        } else {
            ESP_LOGW(TAG, "static addressing is on but the address is not valid; using DHCP");
        }
    }

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start");

    kvm_net_advertise(hostname);
    return ESP_OK;
}
#else
esp_err_t ethernet_init(void)
{
    ESP_LOGW(TAG, "Ethernet disabled (enable KVM_BOARD_ETH_ENABLE for HTTP)");
    return ESP_OK;
}

void kvm_eth_link(bool *up, int *mbps)
{
    if (up) {
        *up = false;
    }
    if (mbps) {
        *mbps = 0;
    }
}
#endif
