/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which pins this board's own hardware already holds.
 *
 * The list used to be built inline where the pins API is served, which meant
 * only the console could see it. It is here now because the settings code needs
 * the same answer: an operator typing a GPIO number into the display or the ATX
 * buttons must be told when that pin is the Ethernet PHY's, not left to find
 * out when the network stops.
 *
 * Every entry reads the Kconfig value the driver itself uses, so the two cannot
 * drift apart. A peripheral this build does not have leaves its pin at -1 and
 * the lookup skips it.
 */
#include "kvm_board_header.h"

#include "sdkconfig.h"

/* D2 and D3 exist only on a 4-bit SDIO bus. */
#if CONFIG_KVM_WIFI && !defined(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D2)
#define CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D2 -1
#define CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D3 -1
#endif

static const kvm_board_reserved_t s_reserved[] = {
    {CONFIG_KVM_I2C_SDA_GPIO, "Capture I2C SDA"},
    {CONFIG_KVM_I2C_SCL_GPIO, "Capture I2C SCL"},
    {CONFIG_KVM_TC358743_RST_GPIO, "Capture reset"},
    {CONFIG_KVM_SD_CLK_GPIO, "microSD CLK"},
    {CONFIG_KVM_SD_CMD_GPIO, "microSD CMD"},
    {CONFIG_KVM_SD_D0_GPIO, "microSD D0"},
    {CONFIG_KVM_SD_D1_GPIO, "microSD D1"},
    {CONFIG_KVM_SD_D2_GPIO, "microSD D2"},
    {CONFIG_KVM_SD_D3_GPIO, "microSD D3"},
    {CONFIG_KVM_SD_PWR_GPIO, "microSD power"},
    {CONFIG_KVM_SD_CD_GPIO, "microSD card detect"},
    {CONFIG_KVM_BRIDGE_INT_GPIO, "Capture interrupt"},
#if CONFIG_KVM_ETH_ENABLE
    {CONFIG_KVM_ETH_RMII_CLK_GPIO, "Ethernet REFCLK"},
    {CONFIG_KVM_ETH_RMII_TX_EN_GPIO, "Ethernet TX_EN"},
    {CONFIG_KVM_ETH_RMII_TXD0_GPIO, "Ethernet TXD0"},
    {CONFIG_KVM_ETH_RMII_TXD1_GPIO, "Ethernet TXD1"},
    {CONFIG_KVM_ETH_RMII_CRS_DV_GPIO, "Ethernet CRS_DV"},
    {CONFIG_KVM_ETH_RMII_RXD0_GPIO, "Ethernet RXD0"},
    {CONFIG_KVM_ETH_RMII_RXD1_GPIO, "Ethernet RXD1"},
    {CONFIG_KVM_ETH_MDC_GPIO, "Ethernet MDC"},
    {CONFIG_KVM_ETH_MDIO_GPIO, "Ethernet MDIO"},
    {CONFIG_KVM_ETH_PHY_RST_GPIO, "Ethernet PHY reset"},
#endif
    {CONFIG_KVM_BUTTON_GPIO, "BOOT button"},
#if CONFIG_ESP_CONSOLE_UART_DEFAULT
    {37, "Console UART TX"},
    {38, "Console UART RX"},
#endif
#if CONFIG_KVM_WIFI
    /* The SDIO link to the WiFi co-processor. These are held by esp-hosted
     * rather than by anything of ours, which is exactly why they were missing
     * here to begin with: nothing in our own config mentions them, so the
     * console offered them as free pins and picking one silently killed WiFi.
     * The pins esp-hosted uses, not the bottom of the range it allows: on a
     * board with no preset (the JetKVM Mini W) the range is 0..100. */
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CLK, "WiFi co-processor CLK"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CMD, "WiFi co-processor CMD"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D0, "WiFi co-processor D0"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1, "WiFi co-processor D1"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D2, "WiFi co-processor D2"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D3, "WiFi co-processor D3"},
    {CONFIG_ESP_HOSTED_HOST_RESET_GPIO, "WiFi co-processor reset"},
#if CONFIG_KVM_BOARD_WAVESHARE_WIFI6_DEVKIT || CONFIG_KVM_BOARD_WAVESHARE_WIFI6 || \
    CONFIG_KVM_BOARD_FIREBEETLE2_P4 || CONFIG_KVM_BOARD_WAVESHARE_MODULE_DEVKIT || \
    CONFIG_KVM_BOARD_VIEWE_P4_PI || CONFIG_KVM_BOARD_WAVESHARE_NANO_WIFI6_DB || \
    CONFIG_KVM_BOARD_FUNCEV
    /* The schematic ties P4 GPIO 6 to the co-processor's IO2 (its wake line). esp-hosted
     * does not claim it, but offering it as free would let something an owner
     * plugs in fight the co-processor. */
    {6, "WiFi co-processor IO2"},
#endif
    /*
     * GPIO 45 carries SD_PWRn on the Function EV board. Our firmware never
     * drives it (the slot is always powered, so KVM_SD_PWR_GPIO is -1), but the
     * net is still attached to the pin unless a resistor is moved - so anything
     * else wired there fights the SD power circuit and never reaches a clean
     * logic level. Found the hard way: it was this driver's default DC pin, and
     * a panel on it stayed dark.
     */
#if !CONFIG_KVM_BOARD_JETKVM_MINI_W /* there 45 is the card-detect switch */
    {45, "SD_PWRn (needs a resistor move to free)"},
#endif
#endif
#if CONFIG_KVM_BOARD_JETKVM_MINI || CONFIG_KVM_BOARD_JETKVM_MINI_W
    /* The built-in screen. Its disp_* pin settings are hidden on this board
       (KVM_DISP_FIXED), and a hidden setting is not weighed by the pin check,
       so the pins are held here instead. */
    {CONFIG_KVM_DISP_MOSI_GPIO, "Screen MOSI"},
    {CONFIG_KVM_DISP_SCLK_GPIO, "Screen SCLK"},
    {CONFIG_KVM_DISP_DC_GPIO, "Screen DC"},
    {CONFIG_KVM_DISP_RST_GPIO, "Screen RST"},
    {CONFIG_KVM_DISP_CS_GPIO, "Screen CS"},
    {CONFIG_KVM_DISP_BL_GPIO, "Screen backlight"},
#if CONFIG_KVM_BTN1_FIXED
    /* btn_gpio is hidden on this board, so the pin check does not weigh it. */
    {CONFIG_KVM_BTN_GPIO_DEFAULT, "Front button SW3"},
#endif
#if CONFIG_KVM_BOARD_JETKVM_MINI_W
    {7, "WiFi co-processor BOOT"},
#endif
#endif
};

const kvm_board_reserved_t *kvm_board_reserved(size_t *count)
{
    *count = sizeof(s_reserved) / sizeof(s_reserved[0]);
    return s_reserved;
}

const char *kvm_board_reserved_by(int gpio)
{
    if (gpio < 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_reserved) / sizeof(s_reserved[0]); i++) {
        if (s_reserved[i].gpio == gpio) {
            return s_reserved[i].use;
        }
    }
    return NULL;
}
