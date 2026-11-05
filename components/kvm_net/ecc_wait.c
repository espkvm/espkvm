/*
 * The ECC accelerator with a time limit.
 *
 * IDF's driver starts the accelerator and waits for "done" with no limit.
 * Once in about 15 boots a P-384 multiply never finished: the Tailscale relay
 * task spun there checking the relay's certificate chain until the task
 * watchdog restarted the device. These two replace IDF's esp_ecc_point_multiply
 * and esp_ecc_point_verify (same steps, see components/mbedtls/port/ecc/
 * esp_ecc.c) and give up after ECC_WAIT_US. The next use powers the block up
 * and resets it, the caller's handshake fails, and the relay connects again.
 * A P-384 multiply takes a few ms.
 */
#include <string.h>

/* First: ecc_impl.h sizes ecc_point_t by SOC_ECC_SUPPORT_CURVE_P384. */
#include "soc/soc_caps.h"
#include "ecc_impl.h"
#include "esp_crypto_lock.h"
#include "esp_crypto_periph_clk.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/ecc_hal.h"

_Static_assert(MAX_SIZE == P384_LEN, "ecc_point_t must match IDF's");

#define ECC_WAIT_US (500 * 1000)

static const char *TAG = "ecc";

static bool len_ok(uint16_t len)
{
    return len == P192_LEN || len == P256_LEN
#if SOC_ECC_SUPPORT_CURVE_P384
           || len == P384_LEN
#endif
        ;
}

static bool wait_done(uint16_t len)
{
    int64_t end = esp_timer_get_time() + ECC_WAIT_US;
    while (!ecc_hal_is_calc_finished()) {
        if (esp_timer_get_time() > end) {
            ESP_LOGE(TAG, "the ECC accelerator did not finish (P-%u), giving up", len * 8);
            return false;
        }
    }
    return true;
}

int __wrap_esp_ecc_point_multiply(const ecc_point_t *point, const uint8_t *scalar, ecc_point_t *result,
                                  bool verify_first)
{
    uint16_t len = point->len;
    if (!len_ok(len)) {
        return -1;
    }
    esp_crypto_ecc_lock_acquire();
    esp_crypto_ecc_enable_periph_clk(true);

    ecc_hal_write_mul_param(scalar, point->x, point->y, len);
    ecc_hal_set_mode(verify_first ? ECC_MODE_VERIFY_THEN_POINT_MUL : ECC_MODE_POINT_MUL);
    ecc_hal_enable_constant_time_point_mul(true);
    ecc_hal_start_calc();
    memset(result, 0, sizeof(ecc_point_t));
    result->len = len;

    int ret = wait_done(len) ? ecc_hal_read_mul_result(result->x, result->y, len) : -1;

    esp_crypto_ecc_enable_periph_clk(false);
    esp_crypto_ecc_lock_release();
    return ret;
}

int __wrap_esp_ecc_point_verify(const ecc_point_t *point)
{
    if (!len_ok(point->len)) {
        return 0;
    }
    esp_crypto_ecc_lock_acquire();
    esp_crypto_ecc_enable_periph_clk(true);

    ecc_hal_write_verify_param(point->x, point->y, point->len);
    ecc_hal_set_mode(ECC_MODE_VERIFY);
    ecc_hal_start_calc();

    int ok = wait_done(point->len) ? ecc_hal_read_verify_result() : 0;

    esp_crypto_ecc_enable_periph_clk(false);
    esp_crypto_ecc_lock_release();
    return ok;
}
