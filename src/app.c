/* Command layer — see include/app.h. */
#include <string.h>
#include "app.h"
#include "store.h"
#include "power.h"
#include "led.h"
#include "ota.h"
#include "bq25180.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "APP";

const char *app_reason(app_err_t e)
{
    switch (e) {
    case APP_OK:                return "";
    case APP_BUSY:              return "busy";
    case APP_NOT_RECORDING:     return "not_recording";
    case APP_STORAGE_FULL:      return "storage_full";
    case APP_BATTERY_LOW:       return "battery_low";
    case APP_BAD_ARGS:          return "bad_args";
    case APP_UNKNOWN_RECORDING: return "unknown_recording";
    default:                    return "error";
    }
}

static app_err_t done(const char *cmd, const char *by, app_err_t e)
{
    if (e == APP_OK) ESP_LOGI(TAG, "%s by %s -> ok", cmd, by ? by : "?");
    else             ESP_LOGW(TAG, "%s by %s -> %s", cmd, by ? by : "?", app_reason(e));
    return e;
}

static app_err_t from_store(esp_err_t e)
{
    if (e == ESP_OK)                return APP_OK;
    if (e == ESP_ERR_INVALID_STATE) return APP_BUSY;
    if (e == ESP_ERR_NO_MEM)        return APP_STORAGE_FULL;
    if (e == ESP_ERR_NOT_FOUND)     return APP_UNKNOWN_RECORDING;
    return APP_ERROR;
}

/* Same rule the backend command always had: refuse on battery below 5 %. With a cable in, or with no battery
 * detected (soc -1), it is never "low". */
static bool battery_low(void)
{
    bq25180_status_t bq;
    bool plugged = power_charger_status(&bq) == ESP_OK && bq.vin_pgood;
    int pct = power_soc_display();
    return !plugged && pct >= 0 && pct < 5;
}

app_err_t app_rec_start(const char *rec_tag, const char *by, char rec_id_out[32])
{
    if (rec_tag && strlen(rec_tag) > APP_TAG_MAX) return done("rec_start", by, APP_BAD_ARGS);
    if (battery_low())                            return done("rec_start", by, APP_BATTERY_LOW);
    char rid[32] = "";
    app_err_t e = from_store(store_start(rec_tag ? rec_tag : "", by, rid));
    if (e == APP_OK && rec_id_out) strlcpy(rec_id_out, rid, 32);
    return done("rec_start", by, e);
}

app_err_t app_rec_stop(const char *by, const char *expect_rec_id, store_rec_t *out)
{
    store_status_t ss; store_get_status(&ss);
    if (ss.state == REC_IDLE) return done("rec_stop", by, APP_NOT_RECORDING);
    if (expect_rec_id && strcmp(expect_rec_id, ss.cur.rec_id) != 0) return done("rec_stop", by, APP_BAD_ARGS);
    store_rec_t tmp;
    esp_err_t e = store_stop(by, out ? out : &tmp);
    return done("rec_stop", by, e == ESP_OK ? APP_OK : APP_NOT_RECORDING);   /* stopped meanwhile by someone else */
}

app_err_t app_rec_toggle(const char *by)
{
    return store_is_recording() ? app_rec_stop(by, NULL, NULL) : app_rec_start(NULL, by, NULL);
}

app_err_t app_rec_delete(const char *rec_id, const char *by)
{
    if (!rec_id || !*rec_id) return done("rec_delete", by, APP_BAD_ARGS);
    app_err_t e = from_store(store_delete(rec_id));
    return done("rec_delete", by, e);
}

app_err_t app_rec_generate(uint32_t minutes, const char *rec_tag, const char *by)
{
    if (rec_tag && strlen(rec_tag) > APP_TAG_MAX) return done("rec_generate", by, APP_BAD_ARGS);
    return done("rec_generate", by, from_store(store_generate(minutes, rec_tag ? rec_tag : "")));
}

void app_identify(uint32_t seconds, const char *by)
{
    led_identify(seconds ? seconds : 5);
    done("identify", by, APP_OK);
}

void app_reboot(const char *by)
{
    done("reboot", by, APP_OK);
    ota_mark_valid();                     /* a requested restart is not a reason to roll back a fresh image */
    vTaskDelay(pdMS_TO_TICKS(1000));      /* let the caller's answer (HTTP response / MQTT ack) leave */
    esp_restart();
}

void app_power_off(const char *by)
{
    done("power_off", by, APP_OK);
    app_request_power_off(by);            /* main loop: closes a session, cloud_shutdown, ship mode / CHARGE-IDLE */
}
