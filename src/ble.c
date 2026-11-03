/* BLE peripheral, advertising only (0.3.44) — see include/ble.h. */
#include <string.h>
#include <stdio.h>
#include "ble.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";
#define ADV_ITVL_MS 500
static uint8_t s_addr_type;
static volatile bool s_conn;
static char s_name[32];

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = true;
            ESP_LOGI(TAG, "phone connected (handle %d), %u KB internal free", ev->connect.conn_handle,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
        } else {
            ESP_LOGW(TAG, "connect failed (status %d): advertising again", ev->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = false;
        ESP_LOGI(TAG, "phone disconnected (reason 0x%03x): advertising again", ev->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_conn) advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    default: break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name; f.name_len = strlen(s_name); f.name_is_complete = 1;   /* 3 + 2 + 22 = 27 of 31 B */
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields: rc %d", rc); return; }
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MS);   /* slow: less airtime taken from WiFi */
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start: rc %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "address: rc %d", rc); return; }
    uint8_t a[6] = { 0 }; ble_hs_id_copy_addr(s_addr_type, a, NULL);
    ESP_LOGI(TAG, "advertising as '%s' every %d ms, address %02x:%02x:%02x:%02x:%02x:%02x", s_name, ADV_ITVL_MS,
             a[5], a[4], a[3], a[2], a[1], a[0]);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset (reason %d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();                               /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_init(const char *device_id)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), before_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(s_name, sizeof s_name, "Perceptio-%s", device_id ? device_id : "?");
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e)); return e; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), after_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "stack up: cost %d KB internal, %d KB PSRAM; %u KB internal free",
             (int)((before - after) >> 10), (int)((before_ps - after_ps) >> 10), (unsigned)(after >> 10));
    return ESP_OK;
}

bool ble_connected(void) { return s_conn; }
