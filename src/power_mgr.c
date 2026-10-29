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
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "PWR";
static power_mgr_prepare_fn s_prepare;
static power_mgr_temp_fn    s_temp;
static const char *volatile s_off_request;           /* set by the UI/cloud (any task); served by power_mgr_tick() */

void power_mgr_init(power_mgr_prepare_fn prepare, power_mgr_temp_fn board_temp) { s_prepare = prepare; s_temp = board_temp; }

/* Off-state breadcrumb in RTC memory (0.3.34). It survives a reset and deep sleep but NOT ship mode, which cuts the
 * chip's supply: so at the next boot "breadcrumb = deep-sleep fallback" proves ship mode did not engage, and "user had
 * switched off + power-on reset + no breadcrumb" means the power really was cut (ship mode, or battery removed). */
#define PM_MAGIC     0x50574F46u                       /* "PWOF" */
#define PM_SHIP_REQ  1                                 /* ship mode requested, waiting for it to cut the supply */
#define PM_FALLBACK  2                                 /* it did not: deep-sleep fallback entered */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic, s_rtc_state;

static void enter_off(uint32_t fallback_s)             /* never returns */
{
    s_rtc_magic = PM_MAGIC; s_rtc_state = PM_SHIP_REQ;
    bq25180_enter_ship_mode();                         /* on battery: BQ disconnects SYS within ~1 s */
    vTaskDelay(pdMS_TO_TICKS(6000));
    s_rtc_state = PM_FALLBACK;
    ESP_LOGE(TAG, "ship mode did not engage");
    power_deep_sleep_fallback(fallback_s);
}

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
        enter_off(60);                                /* ship mode, else deep sleep (button or 60 s) */
    }
}

void power_mgr_boot(bool plugged)
{
    bool fell_back   = s_rtc_magic == PM_MAGIC && s_rtc_state == PM_FALLBACK;
    bool interrupted = s_rtc_magic == PM_MAGIC && s_rtc_state == PM_SHIP_REQ;
    s_rtc_magic = 0;
    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    if (fell_back)
        ESP_LOGW(TAG, "last power-off fell back to deep sleep: ship mode did NOT engage (woken by %s)",
                 wc == ESP_SLEEP_WAKEUP_TIMER ? "the timer" : wc == ESP_SLEEP_WAKEUP_EXT1 ? "the button / charger" : "a reset");
    else if (interrupted)
        ESP_LOGW(TAG, "last power-off was interrupted while waiting for ship mode");
    if (!config_user_off_get()) return;               /* the user had not switched off: normal boot */
    if (!fell_back && !interrupted && esp_reset_reason() == ESP_RST_POWERON)
        ESP_LOGI(TAG, "last power-off reached ship mode (supply was cut)");
    if (plugged) power_mgr_charge_idle();             /* cable in: charge quietly, never returns */
    if (fell_back && wc == ESP_SLEEP_WAKEUP_TIMER) {  /* the fallback timer, nobody pressed: stay off (fixed in 0.3.34) */
        ESP_LOGW(TAG, "switched off and no button press: back to sleep");
        enter_off(600);
    }
    config_user_off_set(false);                       /* woken by the button (from ship mode or deep sleep): turn on */
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

#define LOWBAT_OFF_S  30                               /* seconds at/below bat_off_mv before switching off */
void power_mgr_battery_tick(bool plugged)
{
    static uint32_t low_s;
    float v = power_vbat_avg(), lim = g_cfg.bat_off_mv / 1000.0f;
    bool low = !plugged && power_battery_present() && v > 1.0f && v <= lim;
    if (!low) {
        if (low_s) ESP_LOGW(TAG, "battery recovered: %.2f V, staying on", v);
        low_s = 0;
        return;
    }
    if (low_s == 0) ESP_LOGW(TAG, "battery low: %.2f V (avg) <= %.2f V: switching off in %d s unless it recovers", v, lim, LOWBAT_OFF_S);
    if (++low_s >= LOWBAT_OFF_S) {
        ESP_LOGW(TAG, "battery low for %d s (%.2f V): switching off to protect the cell", LOWBAT_OFF_S, v);
        power_mgr_off("low_battery");
    }
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
            enter_off(60);
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
