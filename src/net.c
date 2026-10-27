#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "net.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "NET";
static EventGroupHandle_t s_ev;
static volatile bool s_keepalive, s_connected;
static char s_ip[16] = "-", s_ssid[33] = "";
#define LINK_HOOKS_MAX 4
static net_link_fn s_link_hooks[LINK_HOOKS_MAX];
static int s_n_link_hooks;
static void link_notify(bool up) { for (int i = 0; i < s_n_link_hooks; i++) s_link_hooks[i](up); }
static volatile bool s_ap_on;
static volatile bool s_testing;                   /* credential test running: no auto-reconnect, no link hook */
static volatile int  s_last_reason;
static SemaphoreHandle_t s_radio;                 /* one station join at a time: boot join, AP retry, test */
#define AP_IP "192.168.4.1"                       /* esp_netif default for the AP interface */
#define EV_CONNECTED BIT0
#define EV_FAILED    BIT1

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool was = s_connected;
        s_connected = false;
        s_last_reason = d->reason;
        ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        if (was && !s_testing) link_notify(false);
        xEventGroupSetBits(s_ev, EV_FAILED);
        /* with the fallback AP up, the AP retry task reconnects (every 60 s, only while no phone is on the AP):
         * a station reconnect loop hops channels and would keep knocking the phone off the AP */
        if (s_keepalive && !s_testing && !s_ap_on) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        /* DNS: log what DHCP gave us, and keep public resolvers behind it. lwIP asks the servers in order and moves on
         * when one does not answer, so a router whose own DNS is dead (or a DHCP lease without a DNS option) no longer
         * leaves the board unable to resolve the backend ("getaddrinfo() returns 202"). */
        {
            esp_netif_dns_info_t m = { 0 }, b = { 0 }, f = { 0 };
            esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &m);
            esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_BACKUP, &b);
            if (b.ip.u_addr.ip4.addr == 0) {
                b.ip.type = ESP_IPADDR_TYPE_V4; b.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
                esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_BACKUP, &b);
            }
            f.ip.type = ESP_IPADDR_TYPE_V4; f.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
            esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_FALLBACK, &f);
            if (m.ip.u_addr.ip4.addr == 0) {                    /* no DNS in the lease at all */
                m.ip.type = ESP_IPADDR_TYPE_V4; m.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
                esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &m);
                ESP_LOGW(TAG, "DHCP gave no DNS server: using 1.1.1.1");
            }
            wifi_config_t wc; char ss[33] = "?";
            if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) { memcpy(ss, wc.sta.ssid, 32); ss[32] = 0; }
            ESP_LOGI(TAG, "'%s': ip %s gw " IPSTR " dns " IPSTR " / " IPSTR " / 8.8.8.8", ss, s_ip, IP2STR(&e->ip_info.gw),
                     IP2STR(&m.ip.u_addr.ip4), IP2STR(&b.ip.u_addr.ip4));
        }
        if (!s_testing) link_notify(true);          /* subscribers: clock (NTP), cloud (MQTT) — see main */
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
    s_radio = xSemaphoreCreateMutex();
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

int net_scan_list(net_ap_t *out, int max)
{
    if (xSemaphoreTake(s_radio, 0) != pdTRUE) return -ESP_ERR_INVALID_STATE;
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    xSemaphoreGive(s_radio);
    if (e != ESP_OK) return -e;
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *ap = calloc(n ? n : 1, sizeof *ap);
    if (!ap) { esp_wifi_clear_ap_list(); return -ESP_ERR_NO_MEM; }
    uint16_t m = n;
    esp_wifi_scan_get_ap_records(&m, ap);           /* sorted by RSSI, strongest first */
    int k = 0;
    for (int i = 0; i < m && k < max; i++) {
        if (!ap[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (strcmp(out[j].ssid, (char *)ap[i].ssid) == 0) { dup = true; break; }
        if (dup) continue;
        snprintf(out[k].ssid, sizeof out[k].ssid, "%s", (char *)ap[i].ssid);
        out[k].rssi = ap[i].rssi; out[k].channel = ap[i].primary; out[k].open = ap[i].authmode == WIFI_AUTH_OPEN;
        k++;
    }
    free(ap);
    ESP_LOGI(TAG, "scan: %d networks", k);
    return k;
}

static esp_err_t connect_from_list_locked(uint32_t timeout_ms);
esp_err_t net_connect_from_list(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(30000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t e = connect_from_list_locked(timeout_ms);
    xSemaphoreGive(s_radio);
    return e;
}

static esp_err_t connect_from_list_locked(uint32_t timeout_ms)
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
    if (!s_ap_on) esp_wifi_stop();                /* with the fallback AP up the radio must stay on */
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Fallback AP                                                          */
/* ------------------------------------------------------------------ */
static void ap_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED)    ESP_LOGI(TAG, "AP: phone/PC connected");
    if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "AP: phone/PC left");
}

static int ap_clients(void)
{
    wifi_sta_list_t l;
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

/* Retry the saved networks while nobody uses the AP (a station connect attempt moves the radio off the AP channel
 * and would drop a connected phone). */
static void ap_retry_task(void *arg)
{
    /* Lives as long as the AP. Switching the AP off is decided by main (backend reachable again: net_stop_ap). */
    while (s_ap_on) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!s_ap_on) break;
        if (s_connected || s_testing) continue;
        if (ap_clients() > 0) continue;
        if (config_wifi_get()->count == 0) continue;
        ESP_LOGI(TAG, "AP: retrying saved networks");
        net_connect_from_list(8000);
    }
    vTaskDelete(NULL);
}

esp_err_t net_stop_ap(void)
{
    if (!s_ap_on) return ESP_OK;
    if (ap_clients() > 0) return ESP_ERR_INVALID_STATE;     /* someone is using it: keep it */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);         /* keeps the station association */
    if (e != ESP_OK) return e;
    s_ap_on = false;
    ESP_LOGW(TAG, "fallback AP off (on '%s', %s)", s_ssid, s_ip);
    return ESP_OK;
}

esp_err_t net_start_ap(const char *ssid, const char *pass)
{
    static esp_netif_t *ap_if;
    if (s_ap_on) return ESP_OK;
    if (!ap_if) ap_if = esp_netif_create_default_wifi_ap();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_evt, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_evt, NULL);
    wifi_config_t ac = { 0 };
    strncpy((char *)ac.ap.ssid, ssid, sizeof ac.ap.ssid);
    ac.ap.ssid_len = strlen(ssid) > 32 ? 32 : strlen(ssid);
    strncpy((char *)ac.ap.password, pass, sizeof ac.ap.password);
    ac.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ac.ap.channel = 1;
    ac.ap.max_connection = 4;
    ac.ap.pmf_cfg.required = false;
    /* no esp_wifi_stop(): the AP may come up next to a working station link (joined, but no internet) */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ac);
    if (e == ESP_OK) e = esp_wifi_start();            /* no-op when the radio already runs */
    if (e != ESP_OK) { ESP_LOGE(TAG, "AP start: %s", esp_err_to_name(e)); return e; }
    s_ap_on = true;
    ESP_LOGW(TAG, "fallback AP '%s' up — UI at http://" AP_IP "/%s%s%s", ssid, s_connected ? " (station still on '" : "", s_connected ? s_ssid : "", s_connected ? "')" : "");
    xTaskCreatePinnedToCore(ap_retry_task, "ap_retry", 3072, NULL, 3, NULL, 0);
    return ESP_OK;
}

bool net_ap_active(void) { return s_ap_on; }

/* ------------------------------------------------------------------ */
/* Credential test                                                      */
/* ------------------------------------------------------------------ */
#define TEST_HOST "mqtt.padelsense.net"              /* name of the backend broker (cloud.c BROKER_URI) */
static net_test_t s_test;
static char s_test_pass[65];

static const char *reason_text(int r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE: case WIFI_REASON_MIC_FAILURE:
        return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found (out of range or 5 GHz only)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "security mode not supported (WPA3-only?) or password not needed";
    case WIFI_REASON_ASSOC_FAIL: case WIFI_REASON_CONNECTION_FAIL:
        return "the router refused the connection";
    case 0: return "no answer from the network";
    default: return "connection failed";
    }
}

static void test_task(void *arg)
{
    net_test_t *t = &s_test;
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(30000)) != pdTRUE) {
        snprintf(t->error, sizeof t->error, "WiFi busy, try again"); t->state = NT_FAIL; vTaskDelete(NULL);
    }
    /* In fallback-AP mode the current station link (if any) is the broken one: a working new network is kept, not
     * swapped back. Otherwise the test returns to where it was. */
    bool connected_now = s_connected;
    bool was = connected_now && !s_ap_on;
    wifi_config_t prev = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &prev);
    char prev_ssid[33]; strlcpy(prev_ssid, s_ssid, sizeof prev_ssid);
    ESP_LOGW(TAG, "%s '%s'%s%s", t->stay ? "connect" : "test", t->ssid, was ? ": leaving " : "", was ? prev_ssid : "");

    s_testing = true;
    if (connected_now) link_notify(false);
    if (connected_now) { esp_wifi_disconnect(); vTaskDelay(pdMS_TO_TICKS(500)); }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, t->ssid, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, s_test_pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = s_test_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    snprintf(t->step, sizeof t->step, "joining");
    s_last_reason = 0;
    xEventGroupClearBits(s_ev, EV_CONNECTED | EV_FAILED);
    esp_wifi_connect();
    /* one join can fail transiently: allow a second attempt inside 20 s */
    EventBits_t b = 0;
    for (int attempt = 0; attempt < 2 && !(b & EV_CONNECTED); attempt++) {
        b = xEventGroupWaitBits(s_ev, EV_CONNECTED | EV_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (b & EV_CONNECTED) break;
        bool bad_pw = s_last_reason == WIFI_REASON_AUTH_FAIL || s_last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                      s_last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        if (attempt == 0 && !bad_pw) { vTaskDelay(pdMS_TO_TICKS(500)); esp_wifi_connect(); }
        else break;
    }
    bool ok = b & EV_CONNECTED;              /* EV_CONNECTED is set on GOT_IP: association + DHCP both done */
    if (ok) {
        snprintf(t->ip, sizeof t->ip, "%s", s_ip);
        wifi_ap_record_t ap; t->rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        snprintf(t->step, sizeof t->step, "dns");
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
        t->internet = getaddrinfo(TEST_HOST, "443", &hints, &res) == 0 && res;
        if (res) freeaddrinfo(res);
        ESP_LOGW(TAG, "test '%s': OK, ip %s, %d dBm, backend name %s", t->ssid, t->ip, t->rssi, t->internet ? "resolved" : "NOT resolved");
        if (!t->stay && config_wifi_add(t->ssid, s_test_pass) == ESP_OK && config_wifi_save() == ESP_OK) t->saved = true;
    } else {
        t->reason = s_last_reason;
        snprintf(t->error, sizeof t->error, "%s", (b & EV_FAILED) || s_last_reason ? reason_text(s_last_reason) : "no IP address (DHCP timeout)");
        ESP_LOGW(TAG, "test '%s': FAILED (%s, reason %d)", t->ssid, t->error, s_last_reason);
    }
    memset(s_test_pass, 0, sizeof s_test_pass);

    /* back to where we were: the previous network if there was one; with none (fallback AP) a working new network is
     * kept — the AP retry task then sees the link and switches the AP off at its next tick. */
    if (was && !(t->stay && ok)) {
        snprintf(t->step, sizeof t->step, "restoring");
        esp_wifi_disconnect(); vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_set_config(WIFI_IF_STA, &prev);
        strlcpy(s_ssid, prev_ssid, sizeof s_ssid);
        s_testing = false;
        s_keepalive = true;
        xEventGroupClearBits(s_ev, EV_CONNECTED | EV_FAILED);
        esp_wifi_connect();
        xEventGroupWaitBits(s_ev, EV_CONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
        ESP_LOGW(TAG, "test done: back on '%s' (%s)", prev_ssid, s_connected ? s_ip : "not yet connected, retrying");
    } else if (ok) {
        strlcpy(s_ssid, t->ssid, sizeof s_ssid);           /* both 33 B; snprintf confuses -Wformat-truncation here */
        s_testing = false;
        s_keepalive = true;
        link_notify(true);
    } else {
        s_testing = false;
        esp_wifi_disconnect();
    }
    t->step[0] = 0;
    t->state = ok ? NT_OK : NT_FAIL;
    xSemaphoreGive(s_radio);
    vTaskDelete(NULL);
}

static esp_err_t start_job(const char *ssid, const char *pass, bool stay);
esp_err_t net_test_start(const char *ssid, const char *pass) { return start_job(ssid, pass, false); }
esp_err_t net_switch_start(const char *ssid)
{
    const cfg_wifi_list_t *wl = config_wifi_get();
    for (int i = 0; i < wl->count; i++)
        if (ssid && strcmp(wl->list[i].ssid, ssid) == 0) {
            if (s_connected && strcmp(s_ssid, ssid) == 0) return ESP_ERR_INVALID_SIZE;   /* already there */
            return start_job(ssid, wl->list[i].pass, true);
        }
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t start_job(const char *ssid, const char *pass, bool stay)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return ESP_ERR_INVALID_ARG;
    if (pass && *pass && strlen(pass) < 8) return ESP_ERR_INVALID_ARG;          /* WPA2 minimum */
    if (s_test.state == NT_RUNNING) return ESP_ERR_INVALID_STATE;
    memset(&s_test, 0, sizeof s_test);
    snprintf(s_test.ssid, sizeof s_test.ssid, "%s", ssid);
    snprintf(s_test_pass, sizeof s_test_pass, "%s", pass ? pass : "");
    snprintf(s_test.step, sizeof s_test.step, "waiting");
    s_test.stay = stay;
    s_test.state = NT_RUNNING;
    if (xTaskCreatePinnedToCore(test_task, "wifi_test", 4096, NULL, 4, NULL, 0) != pdPASS) {
        s_test.state = NT_FAIL; snprintf(s_test.error, sizeof s_test.error, "no memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_test_get(net_test_t *out) { *out = s_test; }

bool net_is_connected(void) { return s_connected; }
const char *net_ip(void) { return s_connected ? s_ip : s_ap_on ? AP_IP : "-"; }
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

esp_err_t net_add_link_hook(net_link_fn fn)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    if (s_n_link_hooks >= LINK_HOOKS_MAX) return ESP_ERR_NO_MEM;
    s_link_hooks[s_n_link_hooks++] = fn;             /* registered at boot, before the first join */
    return ESP_OK;
}
