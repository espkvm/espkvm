/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Redfish: the small part of the DMTF server-management API that tools such
 * as redfishtool and sushy (OpenStack Ironic, Metal3) use to drive a machine:
 * its power, and a virtual CD from a URL.
 *
 *   /redfish/v1                                   service root
 *   /redfish/v1/Systems/1                         power state, Reset action
 *   /redfish/v1/Managers/1                        this device
 *   /redfish/v1/Managers/1/VirtualMedia/Cd        the virtual drive
 *
 * Off by default (redfish_enable). Requests sign in with HTTP basic auth and
 * a password of its own (redfish_pass), any user name. Not the console's
 * password: a script holding it cannot get past two-factor sign-in, read the
 * screen or change a setting, and it can be changed without touching the
 * console's. Failed attempts count against the same lockout as the console's.
 *
 * Power goes through the ATX wiring when there is one. "On" without it sends
 * Wake-on-LAN and the USB wake signal. InsertMedia is the console's "image at a
 * URL": the target boots it straight from the web server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "capture.h"
#include "ethernet.h"
#include "http_recv.h"
#include "kvm_atx.h"
#include "kvm_auth.h"
#include "kvm_net_guard.h"
#include "kvm_settings.h"
#include "usb_hid.h"
#include "web_priv.h"

static const char *TAG = "redfish";

#define BODY_MAX 2048
#define PATH_MAX_LEN 96
#define URL_MAX 500

#define SYSTEM "/redfish/v1/Systems/1"
#define MANAGER "/redfish/v1/Managers/1"
#define VMEDIA MANAGER "/VirtualMedia/Cd"

/* ---- answers ---------------------------------------------------------- */

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "OData-Version", "4.0");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_obj(httpd_req_t *req, cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

/* The Redfish error shape, with a Base registry message id. */
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *id, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *err = cJSON_AddObjectToObject(root, "error");
    char code[64];
    snprintf(code, sizeof(code), "Base.1.8.%s", id);
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON *info = cJSON_AddArrayToObject(err, "@Message.ExtendedInfo");
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "MessageId", code);
    cJSON_AddStringToObject(one, "Message", message);
    cJSON_AddItemToArray(info, one);
    httpd_resp_set_status(req, status);
    return send_obj(req, root);
}

static esp_err_t send_done(httpd_req_t *req)
{
    common_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static cJSON *resource(const char *type, const char *id, const char *name, const char *path)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "@odata.type", type);
    cJSON_AddStringToObject(o, "@odata.id", path);
    cJSON_AddStringToObject(o, "Id", id);
    cJSON_AddStringToObject(o, "Name", name);
    return o;
}

static void add_link(cJSON *parent, const char *key, const char *path)
{
    cJSON *l = cJSON_AddObjectToObject(parent, key);
    cJSON_AddStringToObject(l, "@odata.id", path);
}

static cJSON *collection(const char *type, const char *name, const char *path, const char *member)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "@odata.type", type);
    cJSON_AddStringToObject(o, "@odata.id", path);
    cJSON_AddStringToObject(o, "Name", name);
    cJSON *m = cJSON_AddArrayToObject(o, "Members");
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "@odata.id", member);
    cJSON_AddItemToArray(m, one);
    cJSON_AddNumberToObject(o, "Members@odata.count", 1);
    return o;
}

static void add_status(cJSON *o, const char *state)
{
    cJSON *s = cJSON_AddObjectToObject(o, "Status");
    cJSON_AddStringToObject(s, "State", state);
    cJSON_AddStringToObject(s, "Health", "OK");
}

/* ---- sign-in ---------------------------------------------------------- */

static bool equal_ct(const char *a, const char *b)
{
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)(i < lb ? b[i] : 0);
    }
    return diff == 0;
}

/* True when the request carries the Redfish password. Otherwise the answer
 * has gone out and *sent holds its result. */
static bool signed_in(httpd_req_t *req, esp_err_t *sent)
{
    const char *want = kvm_setting_str("redfish_pass");
    if (!want[0]) {
        *sent = send_error(req, "401 Unauthorized", "InsufficientPrivilege",
                           "No Redfish password is set (Settings, Security, Redfish)");
        return false;
    }
    if (kvm_auth_attempt_locked()) {
        httpd_resp_set_hdr(req, "Retry-After", "30");
        *sent = send_error(req, "429 Too Many Requests", "GeneralError",
                           "Too many failed sign-ins, try again later");
        return false;
    }
    char hdr[200];
    char decoded[160];
    size_t n = 0;
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncasecmp(hdr, "Basic ", 6) == 0 &&
        mbedtls_base64_decode((unsigned char *)decoded, sizeof(decoded) - 1, &n,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
        decoded[n] = '\0';
        const char *colon = strchr(decoded, ':');
        ok = colon && equal_ct(colon + 1, want);
        if (!ok) {
            kvm_auth_attempt_failed();
            char ip[48];
            kvm_web_peer_ip(httpd_req_to_sockfd(req), ip, sizeof(ip));
            ESP_LOGW(TAG, "wrong password from %s", ip);
        }
    }
    memset(decoded, 0, sizeof(decoded));
    memset(hdr, 0, sizeof(hdr));
    if (!ok) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-KVM Redfish\"");
        *sent = send_error(req, "401 Unauthorized", "InsufficientPrivilege", "Sign in with the Redfish password");
        return false;
    }
    kvm_auth_attempt_succeeded();
    kvm_net_guard_seen(req);
    return true;
}

/* ---- the machine ------------------------------------------------------ */

/* "On"/"Off" from the power LED when one is wired. Without it a picture on
 * HDMI says the machine is on, and no picture says nothing for sure. */
static const char *power_state(void)
{
    kvm_atx_status_t atx;
    kvm_atx_status(&atx);
    if (atx.enabled && atx.have_led) {
        return atx.power_on ? "On" : "Off";
    }
    kvm_video_status_t v;
    capture_status_get(&v);
    return v.signal ? "On" : NULL;
}

static void add_reset_types(cJSON *arr, bool atx)
{
    cJSON_AddItemToArray(arr, cJSON_CreateString("On"));
    if (atx) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("ForceOff"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("GracefulShutdown"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("ForceRestart"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("PushPowerButton"));
    }
}

static esp_err_t get_system(httpd_req_t *req)
{
    kvm_atx_status_t atx;
    kvm_atx_status(&atx);
    cJSON *o = resource("#ComputerSystem.v1_13_0.ComputerSystem", "1", "Target", SYSTEM);
    cJSON_AddStringToObject(o, "SystemType", "Physical");
    const char *ps = power_state();
    if (ps) {
        cJSON_AddStringToObject(o, "PowerState", ps);
    } else {
        cJSON_AddNullToObject(o, "PowerState");
    }
    add_status(o, "Enabled");
    cJSON *links = cJSON_AddObjectToObject(o, "Links");
    cJSON *mgr = cJSON_AddArrayToObject(links, "ManagedBy");
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "@odata.id", MANAGER);
    cJSON_AddItemToArray(mgr, one);
    cJSON *actions = cJSON_AddObjectToObject(o, "Actions");
    cJSON *reset = cJSON_AddObjectToObject(actions, "#ComputerSystem.Reset");
    cJSON_AddStringToObject(reset, "target", SYSTEM "/Actions/ComputerSystem.Reset");
    add_reset_types(cJSON_AddArrayToObject(reset, "ResetType@Redfish.AllowableValues"), atx.enabled);
    return send_obj(req, o);
}

static esp_err_t atx_result(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return send_done(req);
    }
    if (err == ESP_ERR_TIMEOUT) {
        return send_error(req, "409 Conflict", "GeneralError", "A power action is already in progress");
    }
    return send_error(req, "500 Internal Server Error", "GeneralError", "Could not queue the power action");
}

static esp_err_t reset_system(httpd_req_t *req, const char *type)
{
    kvm_atx_status_t atx;
    kvm_atx_status(&atx);
    ESP_LOGW(TAG, "Reset %s", type);
    if (strcmp(type, "On") == 0 || strcmp(type, "ForceOn") == 0) {
        const char *ps = power_state();
        if (ps && strcmp(ps, "On") == 0) {
            return send_done(req);
        }
        if (atx.enabled && atx.have_led) {
            return atx_result(req, kvm_atx_power_click());
        }
        /* No way to tell on from off: a press could turn a running machine
         * off, so wake it the ways that never do. */
        const char *mac = kvm_setting_str("pwr_wol_mac");
        if (mac[0]) {
            (void)kvm_wol_send(mac);
        }
        (void)usb_hid_power_on();
        return send_done(req);
    }
    if (!atx.enabled) {
        return send_error(req, "400 Bad Request", "ActionParameterNotSupported",
                          "Only On works without ATX wiring (Settings, Power)");
    }
    if (strcmp(type, "ForceOff") == 0) {
        return atx_result(req, kvm_atx_power_hold());
    }
    if (strcmp(type, "ForceRestart") == 0) {
        return atx_result(req, kvm_atx_reset());
    }
    if (strcmp(type, "PushPowerButton") == 0) {
        return atx_result(req, kvm_atx_power_click());
    }
    if (strcmp(type, "GracefulShutdown") == 0) {
        const char *ps = power_state();
        if (ps && strcmp(ps, "Off") == 0) {
            return send_done(req);
        }
        return atx_result(req, kvm_atx_power_click());
    }
    return send_error(req, "400 Bad Request", "ActionParameterNotSupported", "This ResetType is not supported");
}

/* ---- the virtual drive ------------------------------------------------ */

static esp_err_t get_vmedia(httpd_req_t *req)
{
    const bool enabled = kvm_setting_bool("msc_enable");
    const char *image = kvm_setting_str("msc_image");
    cJSON *o = resource("#VirtualMedia.v1_3_0.VirtualMedia", "Cd", "Virtual drive", VMEDIA);
    cJSON *types = cJSON_AddArrayToObject(o, "MediaTypes");
    cJSON_AddItemToArray(types, cJSON_CreateString("CD"));
    cJSON_AddItemToArray(types, cJSON_CreateString("DVD"));
    cJSON_AddItemToArray(types, cJSON_CreateString("USBStick"));
    const bool inserted = enabled && image[0];
    if (inserted && strcmp(image, "@url") == 0) {
        /* The link may carry a password or a signed token (msc_url is a secret),
         * so only its scheme, host and path go out. */
        char url[URL_MAX + 1];
        snprintf(url, sizeof(url), "%s", kvm_setting_str("msc_url"));
        url[strcspn(url, "?#")] = '\0';
        char *at = strchr(url, '@');
        char *scheme = strstr(url, "://");
        if (at && scheme && at > scheme && !memchr(scheme + 3, '/', (size_t)(at - scheme - 3))) {
            memmove(scheme + 3, at + 1, strlen(at + 1) + 1);
        }
        const char *slash = strrchr(url, '/');
        cJSON_AddStringToObject(o, "Image", url);
        cJSON_AddStringToObject(o, "ImageName", slash ? slash + 1 : url);
        cJSON_AddStringToObject(o, "ConnectedVia", "URI");
        cJSON_AddBoolToObject(o, "WriteProtected", true);
    } else if (inserted) {
        cJSON_AddNullToObject(o, "Image");
        cJSON_AddStringToObject(o, "ImageName", image);
        cJSON_AddStringToObject(o, "ConnectedVia", "Oem");
    } else {
        cJSON_AddNullToObject(o, "Image");
        cJSON_AddStringToObject(o, "ConnectedVia", "NotConnected");
    }
    cJSON_AddBoolToObject(o, "Inserted", inserted);
    add_status(o, enabled ? "Enabled" : "Disabled");
    cJSON *actions = cJSON_AddObjectToObject(o, "Actions");
    cJSON *ins = cJSON_AddObjectToObject(actions, "#VirtualMedia.InsertMedia");
    cJSON_AddStringToObject(ins, "target", VMEDIA "/Actions/VirtualMedia.InsertMedia");
    cJSON *ej = cJSON_AddObjectToObject(actions, "#VirtualMedia.EjectMedia");
    cJSON_AddStringToObject(ej, "target", VMEDIA "/Actions/VirtualMedia.EjectMedia");
    return send_obj(req, o);
}

static esp_err_t insert_media(httpd_req_t *req, const char *url)
{
    if (!kvm_setting_bool("msc_enable")) {
        return send_error(req, "409 Conflict", "ResourceInStandby",
                          "Virtual media is off: turn on Settings, Storage, Expose virtual media and "
                          "restart the device");
    }
    if ((strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) || strlen(url) > URL_MAX) {
        return send_error(req, "400 Bad Request", "PropertyValueFormatError",
                          "Image must be an http:// or https:// URL of at most 500 characters");
    }
    ESP_LOGW(TAG, "InsertMedia");
    if (kvm_setting_set_str("msc_url", url) != ESP_OK || kvm_setting_set_str("msc_image", "@url") != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "GeneralError", "Could not save the image URL");
    }
    return send_done(req);
}

static esp_err_t eject_media(httpd_req_t *req)
{
    ESP_LOGW(TAG, "EjectMedia");
    if (kvm_setting_str("msc_image")[0] && kvm_setting_set_str("msc_image", "") != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "GeneralError", "Could not eject");
    }
    return send_done(req);
}

/* ---- the tree --------------------------------------------------------- */

static esp_err_t get_root(httpd_req_t *req)
{
    cJSON *o = resource("#ServiceRoot.v1_11_0.ServiceRoot", "RootService", "ESP-KVM", "/redfish/v1");
    cJSON_AddStringToObject(o, "RedfishVersion", "1.11.0");
    add_link(o, "Systems", "/redfish/v1/Systems");
    add_link(o, "Managers", "/redfish/v1/Managers");
    cJSON_AddObjectToObject(o, "Links");
    return send_obj(req, o);
}

static esp_err_t get_manager(httpd_req_t *req)
{
    cJSON *o = resource("#Manager.v1_10_0.Manager", "1", "ESP-KVM", MANAGER);
    cJSON_AddStringToObject(o, "ManagerType", "BMC");
    cJSON_AddStringToObject(o, "Model", "ESP-KVM");
    cJSON_AddStringToObject(o, "FirmwareVersion", esp_app_get_description()->version);
    add_status(o, "Enabled");
    add_link(o, "VirtualMedia", MANAGER "/VirtualMedia");
    cJSON *links = cJSON_AddObjectToObject(o, "Links");
    cJSON *sys = cJSON_AddArrayToObject(links, "ManagerForServers");
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "@odata.id", SYSTEM);
    cJSON_AddItemToArray(sys, one);
    return send_obj(req, o);
}

/* The path without a query or a trailing slash. */
static void clean_path(const char *uri, char *out, size_t cap)
{
    snprintf(out, cap, "%s", uri);
    out[strcspn(out, "?#")] = '\0';
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') {
        out[--n] = '\0';
    }
}

static bool enabled_or_404(httpd_req_t *req, esp_err_t *sent)
{
    if (kvm_setting_bool("redfish_enable")) {
        return true;
    }
    *sent = httpd_resp_send_404(req);
    return false;
}

static esp_err_t redfish_get(httpd_req_t *req)
{
    esp_err_t sent = ESP_OK;
    if (!enabled_or_404(req, &sent)) {
        return sent;
    }
    char path[PATH_MAX_LEN];
    clean_path(req->uri, path, sizeof(path));
    if (strcmp(path, "/redfish") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "v1", "/redfish/v1/");
        return send_obj(req, o);
    }
    /* The service root is open to all, as the standard asks: it names no secret. */
    if (strcmp(path, "/redfish/v1") == 0) {
        return get_root(req);
    }
    if (!signed_in(req, &sent)) {
        return sent;
    }
    if (strcmp(path, "/redfish/v1/Systems") == 0) {
        return send_obj(req, collection("#ComputerSystemCollection.ComputerSystemCollection", "Systems",
                                        "/redfish/v1/Systems", SYSTEM));
    }
    if (strcmp(path, SYSTEM) == 0) {
        return get_system(req);
    }
    if (strcmp(path, "/redfish/v1/Managers") == 0) {
        return send_obj(req, collection("#ManagerCollection.ManagerCollection", "Managers",
                                        "/redfish/v1/Managers", MANAGER));
    }
    if (strcmp(path, MANAGER) == 0) {
        return get_manager(req);
    }
    if (strcmp(path, MANAGER "/VirtualMedia") == 0) {
        return send_obj(req, collection("#VirtualMediaCollection.VirtualMediaCollection", "Virtual media",
                                        MANAGER "/VirtualMedia", VMEDIA));
    }
    if (strcmp(path, VMEDIA) == 0) {
        return get_vmedia(req);
    }
    return send_error(req, "404 Not Found", "ResourceMissingAtURI", "No such resource");
}

/* The body as JSON, or NULL after an error answer has gone out. A Redfish
 * client always sends JSON: anything else, a form from another site among
 * them, is refused before it is read. */
static cJSON *read_body(httpd_req_t *req, esp_err_t *sent)
{
    char ctype[64] = "";
    (void)httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    if (strncasecmp(ctype, "application/json", 16) != 0) {
        *sent = send_error(req, "415 Unsupported Media Type", "UnsupportedMediaType",
                           "Send Content-Type: application/json");
        return NULL;
    }
    if (req->content_len == 0) {
        return cJSON_CreateObject(); /* EjectMedia may come with no body */
    }
    if (req->content_len > BODY_MAX) {
        *sent = send_error(req, "413 Content Too Large", "GeneralError", "The body is too large");
        return NULL;
    }
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        *sent = send_error(req, "500 Internal Server Error", "GeneralError", "No memory");
        return NULL;
    }
    const int got = kvm_recv_all(req, body, (int)req->content_len);
    if (got < 0) {
        free(body);
        *sent = send_error(req, "400 Bad Request", "GeneralError", "Could not read the body");
        return NULL;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        *sent = send_error(req, "400 Bad Request", "MalformedJSON", "The body is not a JSON object");
    }
    return j;
}

static esp_err_t redfish_write(httpd_req_t *req, bool patch)
{
    esp_err_t sent = ESP_OK;
    if (!enabled_or_404(req, &sent)) {
        return sent;
    }
    if (!signed_in(req, &sent)) {
        return sent;
    }
    char path[PATH_MAX_LEN];
    clean_path(req->uri, path, sizeof(path));
    const bool reset = !patch && strcmp(path, SYSTEM "/Actions/ComputerSystem.Reset") == 0;
    const bool insert = !patch && strcmp(path, VMEDIA "/Actions/VirtualMedia.InsertMedia") == 0;
    const bool eject = !patch && strcmp(path, VMEDIA "/Actions/VirtualMedia.EjectMedia") == 0;
    const bool vmedia = patch && strcmp(path, VMEDIA) == 0;
    if (!reset && !insert && !eject && !vmedia) {
        return send_error(req, "405 Method Not Allowed", "OperationNotAllowed",
                          "This resource does not take that method");
    }
    cJSON *j = read_body(req, &sent);
    if (!j) {
        return sent;
    }
    const cJSON *image = cJSON_GetObjectItem(j, "Image");
    const cJSON *inserted = cJSON_GetObjectItem(j, "Inserted");
    if (reset) {
        const cJSON *type = cJSON_GetObjectItem(j, "ResetType");
        sent = cJSON_IsString(type) ? reset_system(req, type->valuestring)
                                    : send_error(req, "400 Bad Request", "PropertyMissing", "ResetType is missing");
    } else if (eject) {
        sent = eject_media(req);
    } else if (cJSON_GetObjectItem(j, "UserName") || cJSON_GetObjectItem(j, "Password")) {
        sent = send_error(req, "400 Bad Request", "ActionParameterNotSupported",
                          "Put the web server's user name and password in the Image URL");
    } else if (insert) {
        sent = cJSON_IsString(image) ? insert_media(req, image->valuestring)
                                     : send_error(req, "400 Bad Request", "PropertyMissing", "Image is missing");
    } else if (cJSON_IsFalse(inserted) || cJSON_IsNull(image)) {
        sent = eject_media(req); /* PATCH {"Image": null} or {"Inserted": false} */
    } else if (cJSON_IsString(image)) {
        sent = insert_media(req, image->valuestring);
    } else {
        sent = send_error(req, "400 Bad Request", "PropertyMissing", "Image is missing");
    }
    cJSON_Delete(j);
    return sent;
}

static esp_err_t redfish_post(httpd_req_t *req)
{
    return redfish_write(req, false);
}

static esp_err_t redfish_patch(httpd_req_t *req)
{
    return redfish_write(req, true);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/redfish", .method = HTTP_GET, .handler = redfish_get},
    {.uri = "/redfish/*", .method = HTTP_GET, .handler = redfish_get},
    {.uri = "/redfish/*", .method = HTTP_POST, .handler = redfish_post},
    {.uri = "/redfish/*", .method = HTTP_PATCH, .handler = redfish_patch},
};

const httpd_uri_t *redfish_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
