#include <string.h>
#include "net.h"
#include "config.h"
#include "clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "NET";
static EventGroupHandle_t s_ev;
static volatile bool s_keepalive, s_connected;
static char s_ip[16] = "-", s_ssid[33] = "";
static net_link_fn s_link_hook;
#define EV_CONNECTED BIT0
#define EV_FAILED    BIT1

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool was = s_connected;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        if (was && s_link_hook) s_link_hook(false);
        xEventGroupSetBits(s_ev, EV_FAILED);
        if (s_keepalive) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        clock_network_up();                          /* NTP as soon as we have an address (also on reconnect) */
        if (s_link_hook) s_link_hook(true);
        xEventGroupSetBits(s_ev, EV_CONNECTED);
    }
}

esp_err_t net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, evt, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

int net_scan(void)
{
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t ap[10]; uint16_t m = 10;
    esp_wifi_scan_get_ap_records(&m, ap);
    for (int i = 0; i < m; i++) ESP_LOGI(TAG, "  %-24s ch%2d %4d dBm", ap[i].ssid, ap[i].primary, ap[i].rssi);
    return n;
}

esp_err_t net_connect_from_list(uint32_t timeout_ms)
{
    const cfg_wifi_list_t *wl = config_wifi_get();
    for (int attempt = 0; attempt < 2; attempt++)       /* two rounds: auth failures are often transient */
    for (int i = 0; i < wl->count; i++) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, wl->list[i].ssid, sizeof wc.sta.ssid);
        strncpy((char *)wc.sta.password, wl->list[i].pass, sizeof wc.sta.password);
        wc.sta.threshold.authmode = wl->list[i].pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        xEventGroupClearBits(s_ev, EV_CONNECTED | EV_FAILED);
        ESP_LOGI(TAG, "trying [%d] '%s' ...", i, wl->list[i].ssid);
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_CONNECTED | EV_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (b & EV_CONNECTED) {
            strncpy(s_ssid, wl->list[i].ssid, sizeof s_ssid - 1);
            s_keepalive = true;
            ESP_LOGI(TAG, "joined '%s', ip %s", s_ssid, s_ip);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "'%s' failed (%s)", wl->list[i].ssid, (b & EV_FAILED) ? "auth / no AP" : "timeout");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));     /* let the resulting DISCONNECTED event drain */
    }
    esp_wifi_stop();
    return ESP_ERR_NOT_FOUND;
}

bool net_is_connected(void) { return s_connected; }
const char *net_ip(void) { return s_connected ? s_ip : "-"; }
const char *net_ssid(void) { return s_ssid; }
int8_t net_rssi(void) { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }
void net_set_power_save(bool on) { esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); }

#if __has_include("mdns.h")
#include "mdns.h"
esp_err_t net_start_mdns(const char *device_name, const char *fw_version)
{
    char host[32]; int n = 0;                     /* "Perceptio 2.0" -> "perceptio-2-0" */
    for (const char *c = device_name; *c && n < 30; c++) {
        char ch = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) host[n++] = ch;
        else if (n && host[n - 1] != '-') host[n++] = '-';
    }
    while (n && host[n - 1] == '-') n--;
    host[n] = 0;
    if (!n) strcpy(host, "perceptio");
    esp_err_t e = mdns_init();
    if (e != ESP_OK) return e;
    mdns_hostname_set(host);
    mdns_instance_name_set(device_name);
    mdns_txt_item_t txt[] = { { "fw", (char *)fw_version }, { "api", "1" }, { "model", "perceptio2" } };
    mdns_service_add(NULL, "_perceptio", "_tcp", 80, txt, 3);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS http://%s.local/", host);
    return ESP_OK;
}
#else
esp_err_t net_start_mdns(const char *device_name, const char *fw_version)
{ ESP_LOGW(TAG, "mdns component not available"); return ESP_ERR_NOT_SUPPORTED; }
#endif

void net_set_link_hook(net_link_fn fn) { s_link_hook = fn; }
