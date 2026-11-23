# ESP-KVM copy of esp_driver_sdmmc

Copied from ESP-IDF v6.1 (`components/esp_driver_sdmmc`, test apps left out).
It overrides the IDF component of the same name. Changes are marked `espkvm:`.

- `sd_trans_sdmmc.c`: reset the DMA engine before every transfer. After a failed
  transfer it stayed on a stale descriptor and the next transfer hung with no
  interrupt, which also stopped the WiFi chip on the other slot.
- `sd_trans_sdmmc.c`: a data error ends the transfer, resets the controller and
  sends CMD12, so the card leaves its receive state.
- `sd_host_sdmmc.c`: a clock update the controller does not take (it is still
  waiting for a failed transfer's data) resets the controller and is sent again
  without the wait. Before, every later command failed with 0x107.
- `sd_trans_sdmmc.c`: the DMA descriptor ring wraps at its size, not at the
  number of descriptors being refilled.
- `sd_host_sdmmc.c`, `sd_host_private.h`: while the other slot runs, a slot
  keeps the controller's host divider and sets its speed with its own card
  divider only. The host divider is shared, so the microSD and the WiFi chip
  used to change each other's clock.

Re-check each against upstream when IDF is bumped.
