/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which HDMI-to-CSI bridge is on the other end of the ribbon.
 *
 * There is one today, the TC358743, and until now the capture path said so out
 * loud: it called that driver by name and knew its I2C address. A second bridge
 * would have meant editing every one of those call sites.
 *
 * So a driver announces itself instead. Each one registers a detect function;
 * the capture path asks this file to find a bridge, and gets back a name to show
 * and a table of operations to drive it with. Adding a driver is adding a
 * component - nothing here has to be edited, and nothing above has to care.
 *
 * The idea is Espressif's, from esp_cam_sensor (issue #34), though not the
 * mechanics: theirs collects the detect functions into a linker section, which
 * needs the section placing by hand and lands a table in whatever spot the
 * linker picks. Registering from a constructor is ordinary C, does the same job
 * for a handful of drivers, and has nothing to go wrong at link time. What a
 * driver does need is the -u line in its CMakeLists (see the TC358743's), or
 * the linker drops an object nobody references and the driver silently is not
 * there.
 *
 * One warning about the operations below: they are the shape of the one driver
 * that exists. When a second arrives, expect this table to be wrong somewhere -
 * an interface with a single implementation is a guess, not a design.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the source is sending, as the bridge measures it. */
typedef struct {
    uint16_t hact;   /**< active pixels per line */
    uint16_t vact;   /**< active lines per frame */
    uint16_t htotal; /**< pixels per line including blanking */
    uint16_t vtotal; /**< lines per frame including blanking */
    uint8_t sys_status;
    bool ddc5v;      /**< the source is powering the DDC line: a cable is attached */
    bool tmds;       /**< a TMDS clock is arriving */
    bool hdmi_mode;  /**< HDMI rather than DVI */
    bool sync;       /**< the bridge has locked to the sync */
    bool interlaced;
    uint8_t hz;      /**< measured refresh rate, 0 when the bridge cannot tell */
    uint32_t audio_hz; /**< HDMI audio sample rate, 0 when there is none */
} kvm_bridge_timings_t;

/**
 * How much this device claims to be, as a monitor.
 *
 * The source picks its output mode from what is offered, and two CSI lanes
 * cannot carry everything a PC would like to send - over the limit the bridge
 * gives a black frame rather than a slower picture - so the offer is how the
 * input is kept inside what the wire can take.
 */
typedef enum {
    KVM_BRIDGE_EDID_FULL = 0,
    KVM_BRIDGE_EDID_1080P30,
    KVM_BRIDGE_EDID_720P,
    KVM_BRIDGE_EDID_1024X768,
    KVM_BRIDGE_EDID_480P,
    /* "full" with 1080p50 first; only a 16-bit capture can take it. */
    KVM_BRIDGE_EDID_FULL_16BPP,
} kvm_bridge_edid_profile_t;

/**
 * What the target's OS calls this "monitor". An empty string or a false
 * has_ keeps the built-in value.
 */
typedef struct {
    char mfr[4];   /**< 3-letter PnP code, A-Z */
    char name[14]; /**< up to 13 printable ASCII characters */
    bool has_product;
    bool has_serial;
    uint16_t product;
    uint32_t serial;
} kvm_bridge_edid_id_t;

/** One HDMI-CEC frame: header (initiator << 4 | destination), opcode, operands. */
typedef struct {
    uint8_t len;
    uint8_t data[16];
} kvm_bridge_cec_frame_t;

/** How the line answered the last frame sent. */
typedef enum {
    KVM_BRIDGE_CEC_TX_NONE = 0, /**< still going, or nothing sent */
    KVM_BRIDGE_CEC_TX_OK,
    KVM_BRIDGE_CEC_TX_NACK,     /**< nobody acknowledged a directed frame */
    KVM_BRIDGE_CEC_TX_ARB_LOST, /**< another initiator won the line */
    KVM_BRIDGE_CEC_TX_ERROR,
} kvm_bridge_cec_tx_t;

/**
 * Everything the capture path does to a bridge.
 *
 * @c dev is the driver's own handle, opaque here. Entries may be NULL where a
 * bridge has nothing to do for that step; the helpers below treat a missing one
 * as "nothing to do" rather than an error, so a simpler chip needs no stubs.
 */
typedef struct {
    esp_err_t (*init_streaming)(void *dev);
    esp_err_t (*set_edid_profile)(void *dev, kvm_bridge_edid_profile_t profile);
    esp_err_t (*set_edid_id)(void *dev, const kvm_bridge_edid_id_t *id);
    void (*set_csi_uyvy422)(void *dev, bool uyvy422);
    esp_err_t (*enable_hdmi_output)(void *dev);
    esp_err_t (*hotplug_reset)(void *dev);
    esp_err_t (*reapply_csi_path)(void *dev);
    esp_err_t (*sys_status)(void *dev, uint8_t *out_st);
    esp_err_t (*get_timings)(void *dev, kvm_bridge_timings_t *out);
    void (*debug_status)(void *dev);
    void (*debug_bridge)(void *dev);
    void (*debug_stall_extras)(void *dev);
    void (*remove)(void *dev);
    /* HDMI-CEC, for a bridge that has a controller and a board that wires pin 13
     * to it. All three or none. */
    esp_err_t (*cec_enable)(void *dev, bool on, uint8_t logical_addr);
    esp_err_t (*cec_transmit)(void *dev, const kvm_bridge_cec_frame_t *f, uint8_t free_bits);
    /** Collect what happened since the last call: a received frame and/or the
     *  verdict on the frame being sent. Never blocks. */
    esp_err_t (*cec_poll)(void *dev, kvm_bridge_cec_frame_t *rx, bool *got_rx,
                          kvm_bridge_cec_tx_t *tx);
} kvm_bridge_ops_t;

/** A bridge that answered. */
typedef struct {
    const char *name; /**< what to call it in a log or the console */
    const kvm_bridge_ops_t *ops;
    void *dev; /**< the driver's handle, passed back to every op */
    /**
     * Whether @c ddc5v in the timings means anything from this chip. A bridge
     * that cannot tell a sleeping source from an unplugged cable leaves it
     * false always, and the capture side must not read that as "nothing is
     * attached".
     */
    bool knows_ddc5v;
    /** The chip sends the HDMI sound out as I2S, for kvm_audio to read. */
    bool i2s_audio;
} kvm_bridge_t;

/**
 * A driver's way in: look for your chip on @p bus and, if it is there, fill
 * @p out. Return ESP_ERR_NOT_FOUND when it is not, and the search moves on.
 */
typedef esp_err_t (*kvm_bridge_detect_fn)(i2c_master_bus_handle_t bus, kvm_bridge_t *out);

/** Add a driver to the search. Called from the macro below, not by hand. */
esp_err_t kvm_bridge_register(const char *name, kvm_bridge_detect_fn fn);

/**
 * Declare a bridge driver.
 *
 * Put this at the bottom of the driver, and add to its CMakeLists:
 *   target_link_libraries(${COMPONENT_LIB} INTERFACE "-u kvm_bridge_reg_<name>")
 * Without that line the linker discards an object nothing references, the
 * constructor never runs, and the driver is simply absent.
 */
#define KVM_BRIDGE_DRIVER(name, detect_fn)                                                         \
    void kvm_bridge_reg_##name(void);                                                              \
    __attribute__((constructor)) void kvm_bridge_reg_##name(void)                                  \
    {                                                                                              \
        (void)kvm_bridge_register(#name, (detect_fn));                                             \
    }

/**
 * Ask every registered driver, in the order they registered, until one answers.
 *
 * @return ESP_ERR_NOT_FOUND when nothing on the bus is a bridge this firmware
 *         knows - which for an operator means the ribbon or the board.
 */
esp_err_t kvm_bridge_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out);

/*
 * Every address that answers on the bus: the count, and into out a printable
 * list like " 0x2b". Bring-up tool - "nothing answered" and "something answered
 * that this firmware does not know" send you to different places.
 */
int kvm_bridge_scan(i2c_master_bus_handle_t bus, char *out, size_t out_len);

/*
 * The same scan, with the board's own chips set apart: an audio codec, a touch
 * controller or an OLED can share the capture bus, and one of them answering
 * says nothing about the capture board. @p unknown gets the rest, @p known
 * names what was recognised ("0x18 ES8311 audio codec"). Returns how many
 * unknown addresses answered.
 */
int kvm_bridge_scan_split(i2c_master_bus_handle_t bus, char *unknown, size_t unknown_len,
                          char *known, size_t known_len);

/** How many drivers registered. For the log line at start-up. */
size_t kvm_bridge_driver_count(void);

/* ---- calling a bridge ----------------------------------------------------
 * Thin enough to be free, and they keep the call sites reading as verbs rather
 * than as pointer arithmetic. A NULL op means the bridge needs nothing done. */

static inline esp_err_t kvm_bridge_init_streaming(const kvm_bridge_t *b)
{
    return b->ops->init_streaming ? b->ops->init_streaming(b->dev) : ESP_OK;
}
static inline esp_err_t kvm_bridge_set_edid_profile(const kvm_bridge_t *b,
                                                    kvm_bridge_edid_profile_t p)
{
    return b->ops->set_edid_profile ? b->ops->set_edid_profile(b->dev, p) : ESP_OK;
}
static inline esp_err_t kvm_bridge_set_edid_id(const kvm_bridge_t *b,
                                               const kvm_bridge_edid_id_t *id)
{
    return b->ops->set_edid_id ? b->ops->set_edid_id(b->dev, id) : ESP_OK;
}
static inline void kvm_bridge_set_csi_uyvy422(const kvm_bridge_t *b, bool uyvy422)
{
    if (b->ops->set_csi_uyvy422) {
        b->ops->set_csi_uyvy422(b->dev, uyvy422);
    }
}
static inline esp_err_t kvm_bridge_enable_hdmi_output(const kvm_bridge_t *b)
{
    return b->ops->enable_hdmi_output ? b->ops->enable_hdmi_output(b->dev) : ESP_OK;
}
/* Whether the bridge can cycle its own hotplug line. One that cannot leaves the
 * board holding the only lever there is, its reset pin. */
static inline bool kvm_bridge_has_hotplug_reset(const kvm_bridge_t *b)
{
    return b->ops->hotplug_reset != NULL;
}
static inline esp_err_t kvm_bridge_hotplug_reset(const kvm_bridge_t *b)
{
    return b->ops->hotplug_reset ? b->ops->hotplug_reset(b->dev) : ESP_OK;
}
static inline esp_err_t kvm_bridge_reapply_csi_path(const kvm_bridge_t *b)
{
    return b->ops->reapply_csi_path ? b->ops->reapply_csi_path(b->dev) : ESP_OK;
}
static inline esp_err_t kvm_bridge_sys_status(const kvm_bridge_t *b, uint8_t *out_st)
{
    return b->ops->sys_status ? b->ops->sys_status(b->dev, out_st) : ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t kvm_bridge_get_timings(const kvm_bridge_t *b, kvm_bridge_timings_t *out)
{
    return b->ops->get_timings ? b->ops->get_timings(b->dev, out) : ESP_ERR_NOT_SUPPORTED;
}
static inline void kvm_bridge_debug_status(const kvm_bridge_t *b)
{
    if (b->ops->debug_status) {
        b->ops->debug_status(b->dev);
    }
}
static inline void kvm_bridge_debug_bridge(const kvm_bridge_t *b)
{
    if (b->ops->debug_bridge) {
        b->ops->debug_bridge(b->dev);
    }
}
static inline void kvm_bridge_debug_stall_extras(const kvm_bridge_t *b)
{
    if (b->ops->debug_stall_extras) {
        b->ops->debug_stall_extras(b->dev);
    }
}
static inline bool kvm_bridge_has_cec(const kvm_bridge_t *b)
{
    return b && b->ops && b->ops->cec_enable && b->ops->cec_transmit && b->ops->cec_poll;
}

static inline void kvm_bridge_remove(kvm_bridge_t *b)
{
    if (b->ops && b->ops->remove) {
        b->ops->remove(b->dev);
    }
    b->dev = NULL;
    b->ops = NULL;
}

/**
 * Whether a reading describes a picture.
 *
 * The counters are latched live and read as nonsense for a few milliseconds
 * while a source changes mode, so this is deliberately strict about what counts
 * as a real mode rather than trusting any non-zero number.
 */
bool kvm_bridge_timings_valid(const kvm_bridge_timings_t *t);

#ifdef __cplusplus
}
#endif
