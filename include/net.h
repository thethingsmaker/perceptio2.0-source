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
esp_err_t   net_stop_ap(void);                 /* ESP_ERR_INVALID_STATE while a phone/PC is connected to it */

/* Credential test for the UI ("Test & save"): joins <ssid> with <pass>, waits for an IP address, resolves the
 * backend host name, then goes back to the previous network (or stays, if there was none). Saves the network only on
 * success. Asynchronous: poll net_test_get(). While connected elsewhere the board leaves that network for ~5-25 s. */
typedef enum { NT_IDLE = 0, NT_RUNNING, NT_OK, NT_FAIL } net_test_state_t;
typedef struct {
    net_test_state_t state;
    char   ssid[33];
    char   step[24];          /* running: "joining", "dhcp", "dns", "restoring" */
    char   error[64];         /* NT_FAIL: human-readable cause */
    int    reason;            /* last WiFi disconnect reason code (0 = none) */
    char   ip[16];
    int8_t rssi;
    bool   internet;          /* backend host name resolved */
    bool   saved;
    bool   stay;              /* "connect now" (net_switch_start): stays on the network when it works */
} net_test_t;
esp_err_t   net_test_start(const char *ssid, const char *pass);   /* ESP_ERR_INVALID_STATE when busy */
void        net_test_get(net_test_t *out);
/* "Connect now" to a saved network: same steps as the test, but on success the board stays there (nothing is saved);
 * on failure it goes back to the previous network. ESP_ERR_NOT_FOUND: not in the saved list. */
esp_err_t   net_switch_start(const char *ssid);
bool        net_ap_active(void);
const char *net_ssid(void);
int8_t      net_rssi(void);
void        net_set_power_save(bool on);
esp_err_t   net_start_mdns(const char *device_name, const char *fw_version);
/* Called from the event loop on IP acquired (true) / link lost (false). */
typedef void (*net_link_fn)(bool up);
esp_err_t   net_add_link_hook(net_link_fn fn);  /* up to 4 subscribers, called in registration order */
