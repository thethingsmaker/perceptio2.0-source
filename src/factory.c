#include <stdio.h>
#include <string.h>
#include "factory.h"
#include "app.h"
#include "ble.h"
#include "button.h"
#include "cloud.h"
#include "config.h"
#include "led.h"
#include "ota.h"
#include "power_mgr.h"
#include "store.h"
#include "upload.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FACTORY";
#define CONFIRM_MS 8000

static volatile bool s_pending;
static factory_scope_t s_scope;
static bool s_force;
static char s_by[12], s_cmd[40];
static bool s_boot_done;                              /* gesture check decided for this boot */

static bool busy(void) { return store_is_recording() || upload_active() || ota_busy(); }

esp_err_t factory_request(factory_scope_t scope, bool force, const char *by, const char *cmd_id)
{
    if (s_pending) return ESP_ERR_INVALID_STATE;
    if (!force && busy()) return ESP_ERR_INVALID_STATE;
    s_scope = scope; s_force = force;
    snprintf(s_by, sizeof s_by, "%s", by ? by : "cloud");
    snprintf(s_cmd, sizeof s_cmd, "%s", cmd_id ? cmd_id : "");
    s_pending = true;
    return ESP_OK;
}

/* Main task only. */
static void execute(factory_scope_t scope, bool force, const char *by, const char *cmd_id)
{
    bool closed = false;
    ESP_LOGW(TAG, "factory reset (%s) by %s%s", scope == FACTORY_ALL ? "all" : "pairing", by, force ? ", forced" : "");
    if (store_is_recording()) {
        if (!force) { ESP_LOGW(TAG, "recording: refused"); return; }
        app_rec_stop("factory_reset", NULL, NULL); closed = true;
    }
    char d[128];
    snprintf(d, sizeof d, "{\"scope\":\"%s\",\"by\":\"%s\",\"cmd_id\":%s%s%s,\"recording_closed\":%s}",
             scope == FACTORY_ALL ? "all" : "pairing", by, *cmd_id ? "\"" : "", *cmd_id ? cmd_id : "null", *cmd_id ? "\"" : "",
             closed ? "true" : "false");
    cloud_event("factory_reset", d);
    if (scope == FACTORY_ALL) cloud_shutdown("factory_reset");   /* flushes ack + event, announces offline (<= 1.2 s) */

    ble_unpair();                                                 /* bond(s) gone, pairing window reopened, phone dropped */
    if (scope == FACTORY_PAIRING) { ESP_LOGW(TAG, "pairing reset done"); return; }

    int erased = 0;
    for (int i = 0; i < store_count(); i++) {                     /* every recording; the inventory shrinks as we go */
        const store_rec_t *r = store_rec(i);
        if (r && store_delete(r->rec_id) == ESP_OK) erased++;
    }
    esp_err_t e = config_factory_reset();                         /* parameters + WiFi list + user_off; keeps the OTA token */
    ESP_LOGW(TAG, "erased: bonds, %d recording(s), WiFi list, parameters (%s) -> rebooting", erased, esp_err_to_name(e));
    led_set(LED_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void factory_tick(void)
{
    if (!s_pending) return;
    s_pending = false;
    execute(s_scope, s_force, s_by, s_cmd);
}

/* The charger sets no WAKE flags for the press that woke it from ship mode (0.3.59 test: an 8 s hold through the boot
 * produced nothing), so the gesture needs a FRESH press: once the LED is blue, press and hold. We wait ≤ 1.2 s for that
 * press to show (WAKE1), then up to 3.5 s more for WAKE2. A normal power-on pays ≤ 1.2 s. Anything pressed earlier
 * than this window is deliberately ignored (a normal boot): arming later would collide with the charger's 10 s cut. */
void factory_boot_check(void)
{
    if (s_boot_done || !power_mgr_button_boot()) return;
    s_boot_done = true;
    (void)button_take();                                          /* flags of the power-on press itself */
    int64_t deadline = esp_timer_get_time() + 1200 * 1000;
    power_btn_t k = 0;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
        k = button_take();
        if (k == BTN_LONG) break;
        if (k == BTN_SHORT) { ESP_LOGI(TAG, "button pressed after power-on: waiting for the 3 s hold"); deadline = esp_timer_get_time() + 3500 * 1000; }
    }
    if (k != BTN_LONG) return;
    ESP_LOGW(TAG, "button held after power-on: factory reset ARMED — release, then press again (>= 0.3 s) within %d s to confirm", CONFIRM_MS / 1000);
    led_identify(CONFIRM_MS / 1000 + 1);                          /* white fast blink */
    int64_t until = esp_timer_get_time() + (int64_t)CONFIRM_MS * 1000;
    while (esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(100));
        /* any press the charger flags (>= 0.3 s; a quick tap sets no flag — 0.3.60 test) confirms, held or not */
        if (button_take()) { ESP_LOGW(TAG, "confirmed"); execute(FACTORY_ALL, true, "button", ""); return; }   /* does not return */
    }
    ESP_LOGW(TAG, "not confirmed: normal boot");
    led_set(LED_BOOT);
}
