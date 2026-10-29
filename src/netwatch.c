#include <stdio.h>
#include "netwatch.h"
#include "net.h"
#include "cloud.h"                                  /* cloud_device_id() for the AP name only */
#include "esp_log.h"

static const char *TAG = "NETWATCH";
static netwatch_ok_fn    s_ok;
static netwatch_ap_up_fn s_ap_up;
static uint32_t          s_bad_s, s_good_s;

void netwatch_init(netwatch_ok_fn backend_ok, netwatch_ap_up_fn on_ap_up) { s_ok = backend_ok; s_ap_up = on_ap_up; }

esp_err_t netwatch_start_ap(const char *why)
{
    if (net_ap_active()) return ESP_OK;
    char ssid[33]; snprintf(ssid, sizeof ssid, "Perceptio-%s", cloud_device_id());
    ESP_LOGW(TAG, "fallback AP: %s", why);
    esp_err_t e = net_start_ap(ssid, NETWATCH_AP_PASSWORD);
    if (e == ESP_OK) { net_set_power_save(false); if (s_ap_up) s_ap_up(); }
    return e;
}

void netwatch_tick_1s(void)
{
    bool ok = s_ok ? s_ok() : net_is_connected();
    if (ok) { s_bad_s = 0; s_good_s++; } else { s_good_s = 0; s_bad_s++; }
    if (!net_ap_active() && s_bad_s == NETWATCH_AP_AFTER_S) {
        char why[64]; snprintf(why, sizeof why, "backend unreachable for %d s (%s)", NETWATCH_AP_AFTER_S,
                               net_is_connected() ? "WiFi up, no internet?" : "no WiFi");
        netwatch_start_ap(why);
    }
    if (net_ap_active() && s_good_s >= 30 && s_good_s % 30 == 0) net_stop_ap();   /* retried every 30 s while a phone is on it */
}

uint32_t netwatch_backend_bad_s(void) { return s_bad_s; }
