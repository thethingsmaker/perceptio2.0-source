#include "power.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "POWER";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_calibrated;
static volatile uint32_t s_int_edges;
static power_button_fn s_hook;

static TaskHandle_t s_int_task;

static void IRAM_ATTR int_isr(void *arg)
{
    s_int_edges++;
    BaseType_t hp = pdFALSE;
    if (s_int_task) vTaskNotifyGiveFromISR(s_int_task, &hp);   /* direct notification: nothing to corrupt */
    if (hp) portYIELD_FROM_ISR();
}

/* core 0: collapse bursts of /INT edges, then read STAT1 once and report button (WAKE) flags only.
 * Charger flags (VDPPM/VINDPM/ILIM...) also pulse /INT and are not maskable — they are ignored here
 * and polled through power_charger_status() instead. */
static void int_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(30));                       /* settle: /INT pulses 256 us per flag, often in bursts */
        ulTaskNotifyTake(pdTRUE, 0);                         /* drop what arrived meanwhile */
        uint8_t w = bq25180_take_wake_flags();              /* latched: survives the status polling elsewhere */
        if (w) {
            power_btn_t k = (w & 0x01) ? BTN_LONG : BTN_SHORT;   /* bit0 WAKE2 = long (3 s), bit1 WAKE1 = short */
            ESP_LOGI(TAG, "button: WAKE %02X (%s)", w, k == BTN_LONG ? "long press" : "short press");
            if (s_hook) s_hook(k);
        }
    }
}

esp_err_t power_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, VBAT_ADC_CH, &ccfg));

    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .chan = VBAT_ADC_CH, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    s_calibrated = adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK;
    ESP_LOGI(TAG, "VBAT ADC %s", s_calibrated ? "eFuse curve-fitting calibration" : "UNCALIBRATED (no eFuse data)");

    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_BQ_INT, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&io);
    xTaskCreatePinnedToCore(int_task, "bq_int", 4096, NULL, 4, &s_int_task, 0);
    gpio_isr_handler_add(PIN_BQ_INT, int_isr, NULL);      /* ISR service lives on core 1 (board_init) */
    esp_err_t e = bq25180_init();
    bq25180_take_wake_flags();                            /* discard flags left by the press that woke us */
    return e;
}

float power_vbat(void)
{
    int acc = 0;
    for (int i = 0; i < 16; i++) { int r = 0; adc_oneshot_read(s_adc, VBAT_ADC_CH, &r); acc += r; }
    int raw = acc / 16, mv = 0;
    if (s_calibrated && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv / 1000.0f * VBAT_DIV_RATIO;
    return raw * (3.3f / 4095.0f) * VBAT_DIV_RATIO;
}

bool power_vbat_calibrated(void) { return s_calibrated; }
esp_err_t power_charger_status(bq25180_status_t *st) { return bq25180_get_status(st); }
uint32_t power_int_edges(void) { return s_int_edges; }
void power_set_button_hook(power_button_fn fn) { s_hook = fn; }

void power_deep_sleep_fallback(uint32_t timer_s)
{
    ESP_LOGE(TAG, "deep sleep fallback: wake on button (IO%d low) or in %lu s", PIN_BQ_INT, (unsigned long)timer_s);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BQ_INT, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

power_btn_t power_poll_button(void)
{
    uint8_t w = bq25180_take_wake_flags();
    if (!w) return 0;
    power_btn_t k = (w & 0x01) ? BTN_LONG : BTN_SHORT;
    ESP_LOGI(TAG, "button (poll): WAKE %02X (%s)", w, k == BTN_LONG ? "long press" : "short press");
    return k;
}

int power_soc_pct(float v, bool charging)
{
    /* typical 1-cell LiPo open-circuit voltage vs SoC */
    static const float ocv[] = { 3.30f, 3.45f, 3.55f, 3.62f, 3.68f, 3.73f, 3.78f, 3.84f, 3.90f, 3.98f, 4.08f, 4.20f };
    static const int   pct[] = {    0,     3,     6,    12,    20,    30,    42,    55,    67,    80,    91,   100 };
    if (charging) v -= 0.06f;                   /* charge current lifts the terminal voltage by roughly I*R */
    if (v <= ocv[0]) return 0;
    for (int i = 1; i < (int)(sizeof ocv / sizeof ocv[0]); i++)
        if (v < ocv[i]) return pct[i - 1] + (int)((v - ocv[i - 1]) / (ocv[i] - ocv[i - 1]) * (pct[i] - pct[i - 1]) + 0.5f);
    return 100;
}
