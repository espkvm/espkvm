#include "wireguard-platform.h"

#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <lwip/sys.h>
#include <esp_system.h>
#include <esp_random.h>
#include <esp_err.h>

#include "crypto.h"

/*
 * Random bytes come straight from the ESP hardware RNG (esp_fill_random).
 *
 * This previously seeded an mbedTLS CTR_DRBG from esp_fill_random, but mbedTLS 4
 * (shipped with ESP-IDF 6) no longer exposes the legacy <mbedtls/entropy.h> and
 * <mbedtls/ctr_drbg.h> APIs - they moved behind PSA - so that path no longer
 * compiles. The DRBG was only ever seeded from esp_fill_random anyway, so using
 * the hardware RNG directly is equivalent and works across IDF versions.
 */
esp_err_t wireguard_platform_init() {
	return ESP_OK;
}

void wireguard_random_bytes(void *bytes, size_t size) {
	esp_fill_random(bytes, size);
}

uint32_t wireguard_sys_now() {
	// Default to the LwIP system time
	return sys_now();
}

void wireguard_tai64n_now(uint8_t *output) {
	// See https://cr.yp.to/libtai/tai64.html
	// 64 bit seconds from 1970 = 8 bytes
	// 32 bit nano seconds from current second

	struct timeval tv;
	gettimeofday(&tv, NULL);

	uint64_t seconds = 0x400000000000000aULL + tv.tv_sec;
	uint32_t nanos = tv.tv_usec * 1000;
	U64TO8_BIG(output + 0, seconds);
	U32TO8_BIG(output + 8, nanos);
}

bool wireguard_is_under_load() {
	return false;
}
// vim: noexpandtab
