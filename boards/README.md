# Board targets

ESP-KVM is built per board (separate binaries), not as one universal image: the
ESP32-P4 rev <3.0 and >=3.0 families are mutually exclusive build targets, and
the fps-relevant H.264 RGB path is a compile-time choice tied to the minimum chip
revision.

## Waveshare ESP32-P4-ETH (chip rev v1.3, 32 MB flash) - default

The default build:

```
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Config: `sdkconfig.defaults` + `partitions.csv` (32 MB, `storage` and `rescue`
both 4 MB). Chip target rev <3.0.

## Espressif ESP32-P4 Function EV Board (chip rev v3.2, 16 MB flash)

An overlay on the common defaults, built into a separate directory so it never
clobbers the default build:

```
idf.py -B build.funcev \
  -D SDKCONFIG=build.funcev/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/funcev_p4.defaults" \
  build

idf.py -B build.funcev -p /dev/ttyACM0 flash
```

Config deltas (in `boards/funcev_p4.defaults`): chip target rev >=3.0
(`REV_MIN_300`, which unlocks the esp_h264 RGB-direct path), 16 MB flash with
`partitions_funcev.csv` (`storage` is 3.875 MB), and the microSD power-gate
disabled. The I2C, microSD and Ethernet management pins (IP101: MDC/MDIO/refclk/
reset/addr) match the Waveshare reference and are inherited. The RMII data pins
and the BOOT button GPIO should be taken from the board schematic and set in the
overlay if a given board differs.
