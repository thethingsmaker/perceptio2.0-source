/* WiFi station: join the first reachable network of the NVS list, keep it alive, mDNS. Core 0. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   net_init(void);                    /* netif, event loop, wifi driver (no radio yet) */
int         net_scan(void);                    /* blocking scan; logs APs; returns count (radio check) */
/* Walk the saved list in order; returns ESP_OK when joined (then stays connected + reconnects on drop). */
esp_err_t   net_connect_from_list(uint32_t per_network_timeout_ms);
bool        net_is_connected(void);
const char *net_ip(void);                      /* "-" when not connected */
const char *net_ssid(void);
int8_t      net_rssi(void);
void        net_set_power_save(bool on);
esp_err_t   net_start_mdns(const char *device_name, const char *fw_version);
/* Called from the event loop on IP acquired (true) / link lost (false). */
typedef void (*net_link_fn)(bool up);
void        net_set_link_hook(net_link_fn fn);
