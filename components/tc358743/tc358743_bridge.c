/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The TC358743 as a bridge driver: how it announces itself, and the thin layer
 * between the generic operations and this chip's own calls.
 *
 * The wrappers exist because C will not let a function taking tc358743_t* be
 * called through a pointer that takes void*, however identical the machine code
 * would be. They are one line each and the compiler folds them away.
 */
#include "kvm_bridge.h"
#include "tc358743.h"
#include "tc358743_hdmi_debug.h"

static esp_err_t op_init_streaming(void *dev)
{
    return tc358743_init_streaming((tc358743_t *)dev);
}

static esp_err_t op_set_edid_profile(void *dev, kvm_bridge_edid_profile_t p)
{
    return tc358743_set_edid_profile((tc358743_t *)dev, (tc358743_edid_profile_t)p);
}

static void op_set_csi_uyvy422(void *dev, bool uyvy422)
{
    tc358743_set_csi_uyvy422((tc358743_t *)dev, uyvy422);
}

static esp_err_t op_enable_hdmi_output(void *dev)
{
    return tc358743_enable_hdmi_output((tc358743_t *)dev);
}

static esp_err_t op_hotplug_reset(void *dev)
{
    return tc358743_hdmi_hotplug_reset((tc358743_t *)dev);
}

static esp_err_t op_reapply_csi_path(void *dev)
{
    return tc358743_reapply_csi_path_after_hdmi((tc358743_t *)dev);
}

static esp_err_t op_sys_status(void *dev, uint8_t *out_st)
{
    return tc358743_sys_status((tc358743_t *)dev, out_st);
}

static esp_err_t op_get_timings(void *dev, kvm_bridge_timings_t *out)
{
    return tc358743_get_timings((tc358743_t *)dev, out);
}

static void op_debug_status(void *dev)
{
    tc358743_debug_status((tc358743_t *)dev);
}

static void op_debug_bridge(void *dev)
{
    tc358743_debug_bridge((tc358743_t *)dev);
}

static void op_debug_stall_extras(void *dev)
{
    tc358743_debug_stall_extras((tc358743_t *)dev);
}

static void op_remove(void *dev)
{
    tc358743_remove((tc358743_t *)dev);
}

static const kvm_bridge_ops_t s_ops = {
    .init_streaming = op_init_streaming,
    .set_edid_profile = op_set_edid_profile,
    .set_csi_uyvy422 = op_set_csi_uyvy422,
    .enable_hdmi_output = op_enable_hdmi_output,
    .hotplug_reset = op_hotplug_reset,
    .reapply_csi_path = op_reapply_csi_path,
    .sys_status = op_sys_status,
    .get_timings = op_get_timings,
    .debug_status = op_debug_status,
    .debug_bridge = op_debug_bridge,
    .debug_stall_extras = op_debug_stall_extras,
    .remove = op_remove,
};

/*
 * Is this chip on the bus?
 *
 * The probe already answers exactly that and says ESP_ERR_NOT_FOUND when the
 * address is silent or the CHIPID is somebody else's, which is what the search
 * wants to hear so it can try the next driver.
 */
static esp_err_t tc358743_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out)
{
    tc358743_t *dev = NULL;
    esp_err_t err = tc358743_probe(bus, NULL, &dev);
    if (err != ESP_OK) {
        return err;
    }
    out->name = "TC358743";
    out->ops = &s_ops;
    out->dev = dev;
    return ESP_OK;
}

KVM_BRIDGE_DRIVER(tc358743, tc358743_detect)
