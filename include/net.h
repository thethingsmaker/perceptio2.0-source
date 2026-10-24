/* WiFi station: join the first reachable network of the NVS list, keep it alive, mDNS. Core 0. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   net_init(void);                    /* netif, event loop, wifi driver (no radio yet) */
int         net_scan(void);                    /* blocking scan; logs APs; returns count (radio check) */
typedef struct { char ssid[33]; int8_t rssi; uint8_t channel; bool open; } net_ap_t;
/* Blocking scan (~2-3 s) for the UI: unique non-hidden SSIDs, strongest first. Returns count, <0 = -esp_err
 * (busy: a connect attempt is running). Works while connected and in fallback-AP mode. */
int         net_scan_list(net_ap_t *out, int max);
/* Walk the saved list in order; returns ESP_OK when joined (then stays connected + reconnects on drop). */
esp_err_t   net_connect_from_list(uint32_t per_network_timeout_ms);
bool        net_is_connected(void);
const char *net_ip(void);                      /* station IP; "192.168.4.1" while only the fallback AP is up; "-" otherwise */
/* Fallback access point (no known network at boot): APSTA mode, WPA2, same web UI at http://192.168.4.1/.
 * A background task retries the saved networks every 60 s while no phone is connected to the AP; on success the AP
 * is switched off and the station link hook fires as usual. */
esp_err_t   net_start_ap(const char *ssid, const char *pass);
bool        net_ap_active(void);
const char *net_ssid(void);
int8_t      net_rssi(void);
void        net_set_power_save(bool on);
esp_err_t   net_start_mdns(const char *device_name, const char *fw_version);
/* Called from the event loop on IP acquired (true) / link lost (false). */
typedef void (*net_link_fn)(bool up);
void        net_set_link_hook(net_link_fn fn);
