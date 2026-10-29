#include <inttypes.h>
#include "power_mgr.h"
#include "power.h"
#include "bq25180.h"
#include "button.h"
#include "config.h"
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "PWR";
static power_mgr_prepare_fn s_prepare;
static power_mgr_temp_fn    s_temp;
static const char *volatile s_off_request;           /* set by the UI/cloud (any task); served by power_mgr_tick() */

void power_mgr_init(power_mgr_prepare_fn prepare, power_mgr_temp_fn board_temp) { s_prepare = prepare; s_temp = board_temp; }

/* power.h: the UI/cloud ask; the main loop does it (same path as the button). */
void app_request_power_off(const char *why) { s_off_request = why ? why : "ui"; }
void power_mgr_tick(void) { if (s_off_request) power_mgr_off(s_off_request); }

void power_mgr_off(const char *why)
{
    ESP_LOGW(TAG, "switching OFF (%s)", why);
    if (s_prepare) s_prepare(why);                    /* recording trailer, backend notice, OTA image confirmed */
    led_set(LED_OFF);
    config_user_off_set(true);                        /* allowed: no capture session is running at this point */
    bq25180_status_t bq; power_charger_status(&bq);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (bq.vin_pgood) {
        esp_restart();                                /* cable in: ship mode cannot engage -> reboot into CHARGE-IDLE */
    } else {
        bq25180_enter_ship_mode();                    /* on battery: BQ disconnects SYS within ~1 s */
        vTaskDelay(pdMS_TO_TICKS(6000));
        ESP_LOGE(TAG, "ship mode did not engage");
        power_deep_sleep_fallback(60);
    }
}

bool power_mgr_charge_gate(float board_c)
{
    static bool paused;
    { bq25180_status_t bq; if (power_charger_status(&bq) == ESP_OK && bq.safety_fault && bq.vin_pgood && !paused) {
        ESP_LOGW(TAG, "charger safety-timer fault: restarting the charge cycle");
        bq25180_restart_charge(); } }
    bool too_hot = board_c > g_cfg.chg_tmax_c, too_cold = board_c < g_cfg.chg_tmin_c;
    if (!paused && (too_hot || too_cold)) {
        paused = true; bq25180_charge_enable(false);
        ESP_LOGW(TAG, "charge PAUSED: board %.1f C outside %ld..%ld C", board_c, (long)g_cfg.chg_tmin_c, (long)g_cfg.chg_tmax_c);
    } else if (paused && board_c < g_cfg.chg_tmax_c - 5 && board_c > g_cfg.chg_tmin_c + 5) {
        paused = false; bq25180_charge_enable(true);
        ESP_LOGW(TAG, "charge resumed: board %.1f C", board_c);
    }
    return paused;
}

void power_mgr_charge_idle(void)
{
    ESP_LOGW(TAG, "CHARGE-IDLE: device is off, cable in. Hold the button 2 s to turn on.");
    (void)button_take();                              /* discard a press from before */
    uint32_t tick = 0;
    for (;;) {
        bq25180_status_t bq; power_charger_status(&bq);
        float board_c = s_temp ? s_temp() : 25.0f;
        bool paused = power_mgr_charge_gate(board_c);

        if (!bq.vin_pgood) {                          /* cable removed while off -> really off */
            ESP_LOGW(TAG, "cable removed -> ship mode");
            led_set(LED_OFF);
            bq25180_enter_ship_mode();
            vTaskDelay(pdMS_TO_TICKS(6000));
            power_deep_sleep_fallback(60);
        }
        if (button_take()) {                          /* any press: turn on */
            ESP_LOGW(TAG, "button -> turning ON");
            config_user_off_set(false);
            led_set(LED_OFF);
            esp_restart();
        }

        led_set(paused ? LED_CHARGE_FAULT : bq.chg == BQ_CHG_DONE ? LED_FULL : LED_CHARGING);

        if (++tick % 50 == 0)                         /* every 2 s */
            ESP_LOGI(TAG, "CHARGE-IDLE  VBAT %.2f V  %-18s ILIM %d THERM %d  board %.1f C  STAT1 %02X /INT edges %" PRIu32 "%s",
                     power_vbat(), bq25180_chg_name(bq.chg), bq.ilim_active, bq.thermreg_active, board_c, bq.stat1, power_int_edges(),
                     paused ? "  (charge paused)" : "");
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
