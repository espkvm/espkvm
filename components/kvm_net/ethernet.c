/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ethernet.h"

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "kvm_settings.h"

static const char *TAG = "net";

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
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start");

    esp_err_t mdns_err = mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
    } else {
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
        /* Advertised under the service the device really answers, so a browser
         * or a discovery tool lands on the working port. */
        mdns_err = mdns_service_add("ESP-KVM", tls ? "_https" : "_http", "_tcp", tls ? 443 : 80,
                                    http_txt, 1);
        if (mdns_err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS service: %s", esp_err_to_name(mdns_err));
        } else {
            ESP_LOGI(TAG, "mDNS: %s://%s.local/", tls ? "https" : "http", hostname);
        }
    }
    return ESP_OK;
}
#else
esp_err_t ethernet_init(void)
{
    ESP_LOGW(TAG, "Ethernet disabled (enable KVM_BOARD_ETH_ENABLE for HTTP)");
    return ESP_OK;
}
#endif
