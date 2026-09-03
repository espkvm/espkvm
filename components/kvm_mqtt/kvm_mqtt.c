/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_mqtt.h. The shape:
 *  - apply() reads the mqtt_* settings and (re)builds the esp-mqtt client. It
 *    swaps the client pointer under a mutex so a publish from the timer or the
 *    event task never touches a client being torn down.
 *  - on connect: publish availability "online", publish Home Assistant discovery
 *    for every entity, subscribe to the command topic, publish state once.
 *  - a periodic timer publishes state every mqtt_interval seconds.
 *  - a command on <base>/cmd/<action> maps to an ATX press, Wake-on-LAN or a
 *    device restart.
 * A last-will on <base>/availability flips the device to "offline" if it drops.
 */
#include "kvm_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "capture.h"
#include "screentext_store.h"
#include "ethernet.h"
#include "kvm_atx.h"
#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "usb_hid.h"
#include "fw_install.h"
#include "video_frame.h"

#define TAG "mqtt"

static SemaphoreHandle_t s_mtx;         /* guards s_client across apply/publish */
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static esp_timer_handle_t s_timer;
static esp_timer_handle_t s_alert_timer;

/* How soon after a phrase appears on the screen Home Assistant hears about it. */
#define ALERT_POLL_US (2 * 1000 * 1000)

/* All derived once per apply() and only read afterwards, so no extra locking. */
static char s_node[8];         /* last three MAC bytes, e.g. "a1b2c3" */
static char s_devid[24];       /* "espkvm_a1b2c3", the HA device/unique-id root */
static char s_base_topic[80];  /* "<mqtt_base>/<node>" */
static char s_avail_topic[96];
static char s_state_topic[96];
static char s_cmd_prefix[96];  /* "<base>/cmd/" */
static char s_disco[32];       /* HA discovery prefix */
static char s_dev_json[224];   /* the shared "device" object */

static void compute_node(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(s_node, sizeof(s_node), "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void pub(const char *topic, const char *payload, int retain)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
    }
    xSemaphoreGive(s_mtx);
}

/* ---- state --------------------------------------------------------------- */

/*
 * Copy @p src into @p dst with the two characters JSON cannot carry escaped.
 *
 * The phrase in an alert is whatever the operator typed - a Windows path, or a
 * quoted button name - and one unescaped quote does not just break that field:
 * Home Assistant fails to parse the whole retained payload, and every sensor on
 * the device goes unavailable at once.
 */
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
        }
        /* Anything below a space would be raw control bytes inside a JSON
           string, which is not JSON at all - and the phrase came off a screen,
           so it is not worth trusting to be printable. One space each. */
        dst[o++] = ((unsigned char)*p < 0x20) ? ' ' : *p;
    }
    dst[o] = '\0';
}

/* Which app slot is running, for the diagnostics sensor. */
static const char *running_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

/* Why the device started last time. A box that quietly reboots in the night is
 * worth seeing in Home Assistant, not only in the log. */
static const char *boot_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "power on";
    case ESP_RST_EXT:        return "reset pin";
    case ESP_RST_SW:         return "software restart";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_DEEPSLEEP:  return "deep sleep";
    default:                 return "unknown";
    }
}

static void build_state(char *b, size_t n)
{
    const int32_t jiggle_s = kvm_setting_int("jiggle_s");
    kvm_video_status_t v;
    capture_status_get(&v);
    kvm_atx_status_t a;
    kvm_atx_status(&a);
    const float t = kvm_thermal_celsius();
    const int viewers = video_frame_viewer_count();
    const video_payload_t pl = video_frame_payload();
    const char *codec = pl == VIDEO_PAYLOAD_H264 ? "h264" : pl == VIDEO_PAYLOAD_JPEG ? "mjpeg" : "none";

    char res[16];
    if (v.signal && v.hres) {
        snprintf(res, sizeof(res), "%ux%u", (unsigned)v.hres, (unsigned)v.vres);
    } else {
        snprintf(res, sizeof(res), "no signal");
    }
    const int t_int = (int)t;
    const unsigned t_dec = (unsigned)((t < 0 ? -t : t) * 10.0f) % 10u;
    const unsigned long long uptime = (unsigned long long)(esp_timer_get_time() / 1000000);
    const unsigned psram_kb = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    /* Static for the same reason the payload below is: this runs on the
       esp_timer task, and the alert now holds every phrase at once. */
    static char alert[SCREENTEXT_ALERT_MAX];
    static char alert_json[SCREENTEXT_ALERT_MAX * 2];
    const bool alerting = screentext_alert_get(alert, sizeof(alert), NULL);
    json_escape(alert_json, sizeof(alert_json), alerting ? alert : "");

    snprintf(b, n,
             "{\"tempC\":%d.%u,\"thermal\":\"%s\",\"viewers\":%d,\"signal\":\"%s\","
             "\"resolution\":\"%s\",\"fps\":%u.%02u,\"codec\":\"%s\",\"kbps\":%u,"
             "\"usb\":\"%s\",\"usbBus\":\"%s\",\"power\":\"%s\",\"uptime\":%llu,"
             "\"psramKb\":%u,"
             "\"screenAlert\":\"%s\",\"screenText\":\"%s\","
             /* The other kind of bad screen: one the reader cannot read at all
                because it is not characters - a stop screen, a blanked output -
                which shows up as one flat colour that stays. Half a minute of
                it is a state rather than a repaint. */
             "\"screenFlat\":\"%s\",\"screenFlatSec\":%u,"
             /* Diagnostics. Internal memory gets both figures: the encoder asks
                for one unbroken block, so "free" alone can look healthy while
                the longest run is too short - which is how H.264 came to refuse
                to start after a spell on MJPEG. */
             "\"internalKb\":%u,\"internalLargestKb\":%u,\"skippedFps\":%u.%02u,"
             "\"slot\":\"%s\",\"bootReason\":\"%s\","
             "\"jiggler\":\"%s\",\"jigglerSec\":%d,\"jigglerNudges\":%u}",
             t_int, t_dec, kvm_thermal_state_name(kvm_thermal_state()), viewers,
             v.signal ? "ON" : "OFF", res, (unsigned)(v.fps_x100 / 100),
             (unsigned)(v.fps_x100 % 100), codec, (unsigned)v.kbps,
             usb_hid_ready() ? "ON" : "OFF", usb_hid_bus_alive() ? "ON" : "OFF",
             a.have_led ? (a.power_on ? "ON" : "OFF") : "OFF",
             uptime, psram_kb, alerting ? "ON" : "OFF", alert_json,
             v.flat_ms >= 30000u ? "ON" : "OFF", (unsigned)(v.flat_ms / 1000u),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(v.skipped_fps_x100 / 100u), (unsigned)(v.skipped_fps_x100 % 100u),
             running_slot(), boot_reason(), jiggle_s > 0 ? "ON" : "OFF", (int)jiggle_s,
             (unsigned)usb_hid_jiggler_nudges());
}

/*
 * The state payload does not go on the stack.
 *
 * It is published from the esp_timer task, which runs on about 3.5 KB, and the
 * payload carries the screen alert - on top of what build_state() already needs
 * for the alert and its escaped copy. A state message is one at a time anyway,
 * so it lives in one static buffer under the lock that already serialises
 * publishing.
 *
 * The size is set by the worst case rather than by a measurement: the alert can
 * hold every phrase on screen at once (SCREENTEXT_ALERT_MAX), and escaping can
 * double it. It used to be 576, which was right when an alert was one short
 * phrase and became too small when the watch started naming all of them - a
 * long list would have been published as truncated, unparseable JSON.
 */
#define STATE_JSON_MAX (SCREENTEXT_ALERT_MAX * 2 + 384)
static void publish_snapshot(void);      /* defined with the discovery helpers below */
static void publish_update_state(bool force);

static void publish_state(void)
{
    if (!s_connected) {
        return;
    }
    static char body[STATE_JSON_MAX];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    build_state(body, sizeof(body));
    if (s_client) {
        esp_mqtt_client_publish(s_client, s_state_topic, body, 0, 1, 1);
    }
    xSemaphoreGive(s_mtx);
}

/*
 * The state timer runs every mqtt_interval seconds, which is right for a
 * temperature and much too slow for "the target just printed kernel panic". So
 * a second timer watches only the alert's sequence number - a mutexed read of
 * one integer - and publishes the state the moment it moves.
 */
static void alert_cb(void *arg)
{
    (void)arg;
    static uint32_t s_seen;
    uint32_t seq = 0;
    screentext_alert_get(NULL, 0, &seq);
    if (seq != s_seen) {
        s_seen = seq;
        publish_state();
        /* A phrase matched. If the operator asked for it, send the screen with
         * it - the alert says what was found, the picture says what it looks
         * like. Only on the way in: clearing an alert needs no photograph. */
        if (kvm_setting_bool("mqtt_snap") && screentext_alert_get(NULL, 0, NULL)) {
            publish_snapshot();
        }
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    publish_state();
    publish_update_state(false);
}

/* ---- Home Assistant discovery ------------------------------------------- */

/* One sensor/binary_sensor config. @p val_tpl is the full value template, e.g.
 * "{{ value_json.tempC }}"; optional fields are NULL when unused. */
static void disco_sensor(const char *comp, const char *obj, const char *name, const char *val_tpl,
                         const char *dev_cla, const char *unit, const char *icon,
                         const char *ent_cat)
{
    char j[512];
    int o = snprintf(j, sizeof(j),
                     "{\"~\":\"%s\",\"name\":\"%s\",\"stat_t\":\"~/state\",\"avty_t\":\"~/availability\","
                     "\"val_tpl\":\"%s\",\"uniq_id\":\"%s_%s\"",
                     s_base_topic, name, val_tpl, s_devid, obj);
    if (dev_cla) o += snprintf(j + o, sizeof(j) - o, ",\"dev_cla\":\"%s\"", dev_cla);
    if (unit) o += snprintf(j + o, sizeof(j) - o, ",\"unit_of_meas\":\"%s\"", unit);
    if (icon) o += snprintf(j + o, sizeof(j) - o, ",\"ic\":\"%s\"", icon);
    if (ent_cat) o += snprintf(j + o, sizeof(j) - o, ",\"ent_cat\":\"%s\"", ent_cat);
    o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_disco, comp, s_devid, obj);
    pub(topic, j, 1);
}

/* One button. @p cmd is the command suffix published to <base>/cmd/<cmd>. */
static void disco_button(const char *obj, const char *name, const char *cmd, const char *icon,
                         const char *ent_cat)
{
    char j[512];
    int o = snprintf(j, sizeof(j),
                     "{\"~\":\"%s\",\"name\":\"%s\",\"cmd_t\":\"~/cmd/%s\",\"avty_t\":\"~/availability\","
                     "\"uniq_id\":\"%s_%s\"",
                     s_base_topic, name, cmd, s_devid, obj);
    if (icon) o += snprintf(j + o, sizeof(j) - o, ",\"ic\":\"%s\"", icon);
    if (ent_cat) o += snprintf(j + o, sizeof(j) - o, ",\"ent_cat\":\"%s\"", ent_cat);
    o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/button/%s/%s/config", s_disco, s_devid, obj);
    pub(topic, j, 1);
}

/* A switch: state comes from the shared JSON, commands go to <base>/cmd/<cmd>
 * as ON or OFF. */
static void disco_switch(const char *obj, const char *name, const char *cmd, const char *val_tpl,
                         const char *icon, const char *ent_cat)
{
    char j[512];
    int o = snprintf(j, sizeof(j),
                     "{\"~\":\"%s\",\"name\":\"%s\",\"stat_t\":\"~/state\",\"cmd_t\":\"~/cmd/%s\","
                     "\"avty_t\":\"~/availability\",\"val_tpl\":\"%s\",\"uniq_id\":\"%s_%s\"",
                     s_base_topic, name, cmd, val_tpl, s_devid, obj);
    if (icon) o += snprintf(j + o, sizeof(j) - o, ",\"ic\":\"%s\"", icon);
    if (ent_cat) o += snprintf(j + o, sizeof(j) - o, ",\"ent_cat\":\"%s\"", ent_cat);
    o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/switch/%s/%s/config", s_disco, s_devid, obj);
    pub(topic, j, 1);
}

/* A number box. The payload is the value itself. */
static void disco_number(const char *obj, const char *name, const char *cmd, const char *val_tpl,
                         int min, int max, const char *unit, const char *icon, const char *ent_cat)
{
    char j[640];
    int o = snprintf(j, sizeof(j),
                     "{\"~\":\"%s\",\"name\":\"%s\",\"stat_t\":\"~/state\",\"cmd_t\":\"~/cmd/%s\","
                     "\"avty_t\":\"~/availability\",\"val_tpl\":\"%s\",\"min\":%d,\"max\":%d,"
                     "\"mode\":\"box\",\"uniq_id\":\"%s_%s\"",
                     s_base_topic, name, cmd, val_tpl, min, max, s_devid, obj);
    if (unit) o += snprintf(j + o, sizeof(j) - o, ",\"unit_of_meas\":\"%s\"", unit);
    if (icon) o += snprintf(j + o, sizeof(j) - o, ",\"ic\":\"%s\"", icon);
    if (ent_cat) o += snprintf(j + o, sizeof(j) - o, ",\"ent_cat\":\"%s\"", ent_cat);
    o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/number/%s/%s/config", s_disco, s_devid, obj);
    pub(topic, j, 1);
}

/* An MQTT camera: HA shows whatever JPEG last landed on the topic. */
static void disco_camera(const char *obj, const char *name, const char *icon)
{
    char j[512];
    int o = snprintf(j, sizeof(j),
                     "{\"~\":\"%s\",\"name\":\"%s\",\"t\":\"~/snapshot\","
                     "\"avty_t\":\"~/availability\",\"uniq_id\":\"%s_%s\"",
                     s_base_topic, name, s_devid, obj);
    if (icon) o += snprintf(j + o, sizeof(j) - o, ",\"ic\":\"%s\"", icon);
    o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/camera/%s/%s/config", s_disco, s_devid, obj);
    pub(topic, j, 1);
}

/*
 * Publish one still of the target's screen.
 *
 * Only possible while MJPEG is the codec - H.264 has no still to hand out, the
 * same reason /api/v1/video/frame.jpg refuses there. Published at QoS 0 and not
 * retained: the client sends a large message in fragments rather than copying it
 * whole, which matters on a chip with this little internal memory, and a stale
 * screenshot sitting on the broker forever helps nobody.
 */
static void publish_snapshot(void)
{
    if (!s_connected || !s_client) {
        return;
    }
    if (video_frame_payload() != VIDEO_PAYLOAD_JPEG) {
        ESP_LOGI(TAG, "snapshot skipped: needs the MJPEG codec");
        return;
    }
    video_frame_viewer_enter();
    (void)video_frame_wait_new(video_frame_seq(), 2000);
    video_frame_ref_t ref;
    if (video_frame_acquire(&ref)) {
        if (ref.payload == VIDEO_PAYLOAD_JPEG && ref.len > 0) {
            char topic[96];
            snprintf(topic, sizeof(topic), "%s/snapshot", s_base_topic);
            esp_mqtt_client_publish(s_client, topic, (const char *)ref.data, (int)ref.len, 0, 0);
            ESP_LOGI(TAG, "snapshot published (%u bytes)", (unsigned)ref.len);
        }
        video_frame_release(&ref);
    }
    video_frame_viewer_leave();
}

static void publish_discovery(void)
{
    disco_sensor("sensor", "temp", "Temperature", "{{ value_json.tempC }}", "temperature", "°C",
                 NULL, NULL);
    disco_sensor("sensor", "viewers", "Viewers", "{{ value_json.viewers }}", NULL, NULL,
                 "mdi:account-eye", NULL);
    disco_sensor("sensor", "fps", "Frame rate", "{{ value_json.fps }}", NULL, "fps", "mdi:video",
                 NULL);
    disco_sensor("sensor", "res", "Resolution", "{{ value_json.resolution }}", NULL, NULL,
                 "mdi:monitor", NULL);
    disco_sensor("sensor", "codec", "Codec", "{{ value_json.codec }}", NULL, NULL,
                 "mdi:video-input-hdmi", "diagnostic");
    disco_sensor("sensor", "kbps", "Bitrate", "{{ value_json.kbps }}", NULL, "kbit/s", NULL,
                 "diagnostic");
    disco_sensor("sensor", "uptime", "Uptime", "{{ value_json.uptime }}", "duration", "s", NULL,
                 "diagnostic");
    disco_sensor("sensor", "psram", "Free PSRAM", "{{ value_json.psramKb }}", NULL, "kB", NULL,
                 "diagnostic");
    disco_sensor("binary_sensor", "alert", "Screen alert", "{{ value_json.screenAlert }}",
                 "problem", NULL, "mdi:message-alert", NULL);
    disco_sensor("sensor", "alerttext", "Screen alert text", "{{ value_json.screenText }}", NULL,
                 NULL, "mdi:text-recognition", "diagnostic");
    disco_sensor("binary_sensor", "flat", "Screen one colour", "{{ value_json.screenFlat }}",
                 "problem", NULL, "mdi:square-rounded", NULL);
    disco_sensor("sensor", "flatsec", "Screen one colour for", "{{ value_json.screenFlatSec }}",
                 "duration", "s", "mdi:timer-outline", "diagnostic");
    disco_sensor("binary_sensor", "signal", "HDMI signal", "{{ value_json.signal }}", NULL, NULL,
                 "mdi:hdmi-port", NULL);
    disco_sensor("binary_sensor", "usb", "Target USB", "{{ value_json.usb }}", "connectivity", NULL,
                 NULL, NULL);
    /* The other half of a dead keyboard, and the half worth automating on: with
       no power on the target's port there is nothing to re-plug into, and the
       fix is at the target - a machine that slept and took its port down with
       it is a thing to be told about rather than to discover a day later. */
    disco_sensor("binary_sensor", "usbbus", "Target USB port power", "{{ value_json.usbBus }}",
                 "connectivity", NULL, "mdi:usb-port", "diagnostic");

    if (kvm_cap_available(KVM_CAP_ATX)) {
        disco_sensor("binary_sensor", "power", "Target power", "{{ value_json.power }}", "power",
                     NULL, NULL, NULL);
        disco_button("btn_power", "Power button", "power", "mdi:power", NULL);
        disco_button("btn_reset", "Reset", "reset", "mdi:restart-alert", NULL);
        disco_button("btn_forceoff", "Force off (hold)", "forceoff", "mdi:power-plug-off", NULL);
    }
    if (kvm_cap_available(KVM_CAP_WOL) && kvm_setting_str("pwr_wol_mac")[0]) {
        disco_button("btn_wol", "Wake on LAN", "wol", "mdi:lan-connect", NULL);
    }
    disco_button("btn_restart", "Restart ESP-KVM", "restart", "mdi:restart", "diagnostic");

    /* The jiggler, as a switch and the interval beside it: the point of having
     * it here is an automation - quiet during the day, awake at night. */
    disco_switch("jiggler", "Mouse jiggler", "jiggler", "{{ value_json.jiggler }}",
                 "mdi:mouse-move-vertical", NULL);
    disco_number("jiggler_s", "Jiggle every", "jiggler_s", "{{ value_json.jigglerSec }}", 0, 3600,
                 "s", "mdi:timer-outline", "config");
    disco_sensor("sensor", "nudges", "Jiggler nudges", "{{ value_json.jigglerNudges }}", NULL, NULL,
                 "mdi:counter", "diagnostic");

    /* Diagnostics. Both internal-memory figures, because the gap between them is
     * what decides whether the H.264 encoder can start. */
    disco_sensor("sensor", "internal", "Free internal RAM", "{{ value_json.internalKb }}", NULL,
                 "kB", NULL, "diagnostic");
    disco_sensor("sensor", "internal_big", "Largest internal block",
                 "{{ value_json.internalLargestKb }}", NULL, "kB", NULL, "diagnostic");
    disco_sensor("sensor", "skipped", "Skipped frames", "{{ value_json.skippedFps }}", NULL, "fps",
                 NULL, "diagnostic");
    disco_sensor("sensor", "slot", "Firmware slot", "{{ value_json.slot }}", NULL, NULL,
                 "mdi:chip", "diagnostic");
    disco_sensor("sensor", "bootreason", "Last boot", "{{ value_json.bootReason }}", NULL, NULL,
                 "mdi:restart", "diagnostic");

    /* A still of the target's screen, and the button that asks for one. The
     * picture is what makes a "kernel panic" notification worth opening. */
    disco_camera("screen", "Target screen", "mdi:monitor-screenshot");
    disco_button("btn_snap", "Take a screenshot", "snapshot", "mdi:camera", NULL);

    /* Firmware updates, but only where the device is allowed to look: with
     * fw_fetch off it cannot see what has been published, and an update entity
     * that never knows the answer is worse than none. */
    if (kvm_setting_bool("fw_fetch")) {
        char j[640];
        int o = snprintf(j, sizeof(j),
                         "{\"~\":\"%s\",\"name\":\"Firmware\",\"stat_t\":\"~/update\","
                         "\"cmd_t\":\"~/cmd/install\",\"avty_t\":\"~/availability\","
                         "\"dev_cla\":\"firmware\",\"ent_cat\":\"config\",\"uniq_id\":\"%s_fw\"",
                         s_base_topic, s_devid);
        o += snprintf(j + o, sizeof(j) - o, ",\"dev\":%s}", s_dev_json);
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/update/%s/fw/config", s_disco, s_devid);
        pub(topic, j, 1);
    }
}

/*
 * What is running and what is published, for the update entity.
 *
 * The check goes out at most once every six hours: the answer changes on the
 * day of a release and never in between, and this is a device that is meant to
 * be able to sit on a network with no internet at all.
 */
static void publish_update_state(bool force)
{
    if (!s_connected || !kvm_setting_bool("fw_fetch")) {
        return;
    }
    static int64_t s_checked_us;
    static char s_latest[FW_INSTALL_VERSION_MAX];
    const int64_t now = esp_timer_get_time();
    if (force || s_checked_us == 0 || now - s_checked_us > (int64_t)6 * 3600 * 1000000) {
        s_checked_us = now;
        char latest[FW_INSTALL_VERSION_MAX] = {0};
        if (fw_latest_version(latest, sizeof(latest)) == ESP_OK) {
            snprintf(s_latest, sizeof(s_latest), "%s", latest);
        }
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const char *installed = app ? app->version : "?";
    char j[192];
    snprintf(j, sizeof(j), "{\"installed_version\":\"%s\",\"latest_version\":\"%s\"}",
             installed, s_latest[0] ? s_latest : installed);
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/update", s_base_topic);
    pub(topic, j, 1);
}

/* ---- commands ------------------------------------------------------------ */

/** Does this command's payload say @p want (ON/OFF)? */
static bool payload_is(esp_mqtt_event_handle_t e, const char *want)
{
    const size_t n = strlen(want);
    return e->data_len == (int)n && strncasecmp(e->data, want, n) == 0;
}

static void handle_command(esp_mqtt_event_handle_t e)
{
    /* The topic is not NUL-terminated; copy the small amount we need. */
    char topic[96];
    const int len = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, e->topic, len);
    topic[len] = '\0';

    const size_t plen = strlen(s_cmd_prefix);
    if (strncmp(topic, s_cmd_prefix, plen) != 0) {
        return;
    }
    const char *action = topic + plen;
    ESP_LOGI(TAG, "command: %s", action);

    if (strcmp(action, "power") == 0) {
        kvm_atx_power_click();
    } else if (strcmp(action, "reset") == 0) {
        kvm_atx_reset();
    } else if (strcmp(action, "forceoff") == 0) {
        kvm_atx_power_hold();
    } else if (strcmp(action, "wol") == 0) {
        kvm_wol_send(kvm_setting_str("pwr_wol_mac"));
    } else if (strcmp(action, "restart") == 0) {
        pub(s_avail_topic, "offline", 1);
        vTaskDelay(pdMS_TO_TICKS(300)); /* let the offline notice go out first */
        esp_restart();
    } else if (strcmp(action, "install") == 0) {
        /* HA sends "install" here. The version to install is whatever the
         * manifest last named; fw_install_start does the fetching, the writing
         * and the restart, and refuses if the device may not fetch. */
        char latest[FW_INSTALL_VERSION_MAX] = {0};
        if (fw_latest_version(latest, sizeof(latest)) == ESP_OK) {
            const esp_err_t err = fw_install_start(latest);
            ESP_LOGW(TAG, "install %s requested from Home Assistant: %s", latest,
                     esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "install requested but the manifest could not be read");
        }
    } else if (strcmp(action, "snapshot") == 0) {
        publish_snapshot();
    } else if (strcmp(action, "jiggler") == 0) {
        /* The switch carries no interval, so turning it on restores the last one
         * the operator set - or a minute, which is under every screen lock we
         * have met. Turning it off keeps that number in mind rather than in the
         * setting, so the next ON does not have to be told again. */
        static int32_t s_last_interval;
        const bool on = payload_is(e, "ON");
        const int32_t now = kvm_setting_int("jiggle_s");
        if (!on && now > 0) {
            s_last_interval = now;
        }
        const int32_t want = on ? (s_last_interval > 0 ? s_last_interval : 60) : 0;
        (void)kvm_setting_set_int("jiggle_s", want);
        publish_state();
    } else if (strcmp(action, "jiggler_s") == 0) {
        char v[12];
        const int n = e->data_len < (int)sizeof(v) - 1 ? e->data_len : (int)sizeof(v) - 1;
        memcpy(v, e->data, n);
        v[n] = '\0';
        long secs = strtol(v, NULL, 10);
        if (secs < 0) {
            secs = 0;
        } else if (secs > 3600) {
            secs = 3600;
        }
        (void)kvm_setting_set_int("jiggle_s", (int32_t)secs);
        publish_state();
    } else {
        ESP_LOGW(TAG, "unknown command: %s", action);
    }
}

/* ---- client lifecycle ---------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        pub(s_avail_topic, "online", 1);
        publish_discovery();
        publish_update_state(true);
        char sub[104];
        snprintf(sub, sizeof(sub), "%s/cmd/+", s_base_topic);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_client) {
            esp_mqtt_client_subscribe(s_client, sub, 1);
        }
        xSemaphoreGive(s_mtx);
        publish_state();
        ESP_LOGI(TAG, "connected; discovery and state published");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_command(e);
        break;
    default:
        break;
    }
}

esp_err_t kvm_mqtt_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_timer) {
        const esp_timer_create_args_t args = {.callback = timer_cb, .name = "mqtt_pub"};
        esp_err_t err = esp_timer_create(&args, &s_timer);
        if (err == ESP_OK && !s_alert_timer) {
            const esp_timer_create_args_t aargs = {.callback = alert_cb, .name = "mqtt_alert"};
            if (esp_timer_create(&aargs, &s_alert_timer) == ESP_OK) {
                esp_timer_start_periodic(s_alert_timer, ALERT_POLL_US);
            }
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    compute_node();
    return ESP_OK;
}

esp_err_t kvm_mqtt_apply(void)
{
    if (!s_mtx || !s_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    /* If we are currently connected, say "offline" before tearing down. A
     * graceful disconnect does NOT trigger the last will, so without this a
     * manual disable (or a reconfigure) would leave a stale retained "online"
     * in Home Assistant. Publish on the old topic, then give it a moment to go
     * out before the stop. (A reconfigure republishes "online" right after.) */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool was_connected = s_connected && s_client;
    if (was_connected) {
        esp_mqtt_client_publish(s_client, s_avail_topic, "offline", 0, 1, 1);
    }
    xSemaphoreGive(s_mtx);
    if (was_connected) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    /* Tear down any existing client. Swap the pointer to NULL under the lock so
     * a concurrent publish stops touching it, then stop/destroy outside the lock
     * (client_stop can wait on the mqtt task, which may itself be inside a
     * publish waiting for the lock). */
    esp_timer_stop(s_timer);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_mqtt_client_handle_t old = s_client;
    s_client = NULL;
    s_connected = false;
    xSemaphoreGive(s_mtx);
    if (old) {
        esp_mqtt_client_stop(old);
        esp_mqtt_client_destroy(old);
    }

    if (!kvm_setting_bool("mqtt_enable")) {
        ESP_LOGI(TAG, "disabled");
        return ESP_OK;
    }
    const char *host = kvm_setting_str("mqtt_host");
    if (!host || !host[0]) {
        ESP_LOGW(TAG, "enabled but no broker host set");
        return ESP_OK;
    }

    /* Topics and the shared device object, derived once here. */
    snprintf(s_devid, sizeof(s_devid), "espkvm_%s", s_node);
    snprintf(s_base_topic, sizeof(s_base_topic), "%s/%s", kvm_setting_str("mqtt_base"), s_node);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/availability", s_base_topic);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", s_base_topic);
    snprintf(s_cmd_prefix, sizeof(s_cmd_prefix), "%s/cmd/", s_base_topic);
    snprintf(s_disco, sizeof(s_disco), "%s", kvm_setting_str("mqtt_disco"));

    const char *hostname = kvm_setting_str("net_hostname");
    if (!hostname || !hostname[0]) {
        hostname = "espkvm";
    }
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_dev_json, sizeof(s_dev_json),
             "{\"ids\":[\"%s\"],\"name\":\"ESP-KVM\",\"mdl\":\"ESP32-P4\",\"mf\":\"ESP-KVM\","
             "\"sw\":\"%s\",\"cu\":\"https://%s.local/\"}",
             s_devid, app ? app->version : "?", hostname);

    const bool tls = kvm_setting_bool("mqtt_tls");
    static char uri[96]; /* esp-mqtt copies it, but keep it alive regardless */
    snprintf(uri, sizeof(uri), "%s://%s:%d", tls ? "mqtts" : "mqtt", host,
             (int)kvm_setting_int("mqtt_port"));

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    const char *user = kvm_setting_str("mqtt_user");
    const char *pass = kvm_setting_str("mqtt_pass");
    if (user && user[0]) {
        cfg.credentials.username = user;
    }
    if (pass && pass[0]) {
        cfg.credentials.authentication.password = pass;
    }
    cfg.session.last_will.topic = s_avail_topic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.keepalive = 30;
    /* With TLS and verification on, check the broker against the built-in CA
     * bundle (public CAs). No CA attached => esp-tls does not verify, which is
     * what a self-signed broker needs. */
    if (tls && kvm_setting_bool("mqtt_verify")) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, NULL);

    /* Publish before start: the CONNECTED callback runs on the mqtt task and
     * must find the client pointer already in place. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_mtx);

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start: %s", esp_err_to_name(err));
        return err;
    }

    int32_t interval = kvm_setting_int("mqtt_interval");
    if (interval < 5) {
        interval = 30;
    }
    esp_timer_start_periodic(s_timer, (uint64_t)interval * 1000000ULL);
    ESP_LOGI(TAG, "connecting to %s (tls=%d, verify=%d)", uri, tls,
             tls ? (int)kvm_setting_bool("mqtt_verify") : 0);
    return ESP_OK;
}

void kvm_mqtt_notify(void)
{
    if (s_connected) {
        publish_discovery();
        publish_state();
    }
}

void kvm_mqtt_status(bool *enabled, bool *connected)
{
    bool en = false, conn = false;
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        en = s_client != NULL; /* a client exists only while enabled with a host */
        conn = s_connected;
        xSemaphoreGive(s_mtx);
    }
    if (enabled) {
        *enabled = en;
    }
    if (connected) {
        *connected = conn;
    }
}
