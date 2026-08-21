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
        dst[o++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    dst[o] = '\0';
}

static void build_state(char *b, size_t n)
{
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
    char alert[SCREENTEXT_ALERT_MAX];
    const bool alerting = screentext_alert_get(alert, sizeof(alert), NULL);
    char alert_json[SCREENTEXT_ALERT_MAX * 2];
    json_escape(alert_json, sizeof(alert_json), alerting ? alert : "");

    snprintf(b, n,
             "{\"tempC\":%d.%u,\"thermal\":\"%s\",\"viewers\":%d,\"signal\":\"%s\","
             "\"resolution\":\"%s\",\"fps\":%u.%02u,\"codec\":\"%s\",\"kbps\":%u,"
             "\"usb\":\"%s\",\"power\":\"%s\",\"uptime\":%llu,\"psramKb\":%u,"
             "\"screenAlert\":\"%s\",\"screenText\":\"%s\"}",
             t_int, t_dec, kvm_thermal_state_name(kvm_thermal_state()), viewers,
             v.signal ? "ON" : "OFF", res, (unsigned)(v.fps_x100 / 100),
             (unsigned)(v.fps_x100 % 100), codec, (unsigned)v.kbps,
             usb_hid_ready() ? "ON" : "OFF", a.have_led ? (a.power_on ? "ON" : "OFF") : "OFF",
             uptime, psram_kb, alerting ? "ON" : "OFF", alert_json);
}

static void publish_state(void)
{
    if (!s_connected) {
        return;
    }
    char body[576];
    build_state(body, sizeof(body));
    pub(s_state_topic, body, 1);
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
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    publish_state();
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
    disco_sensor("binary_sensor", "signal", "HDMI signal", "{{ value_json.signal }}", NULL, NULL,
                 "mdi:hdmi-port", NULL);
    disco_sensor("binary_sensor", "usb", "Target USB", "{{ value_json.usb }}", "connectivity", NULL,
                 NULL, NULL);

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
}

/* ---- commands ------------------------------------------------------------ */

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
