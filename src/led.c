#include <math.h>
#include "led.h"
#include "lp5810.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "task_psram.h"

typedef struct { uint8_t r, g, b; float period_s; float floor; } pattern_t;

/* floor = minimum of the breath (fraction of peak) so the LED never goes fully dark */
static const pattern_t PAT[] = {
    [LED_OFF]           = { 0,   0,   0,   1.0f, 0.00f },
    [LED_BOOT]          = { 0,   0,   255, 1.0f, 0.10f },
    [LED_OK]            = { 0,   255, 0,   3.0f, 0.06f },
    [LED_FAULT]         = { 255, 0,   0,   1.0f, 0.15f },
    [LED_CHARGING]      = { 255, 70,  0,   2.5f, 0.05f },
    [LED_FULL]          = { 0,   90,  0,   6.0f, 0.30f },
    [LED_CHARGE_PAUSED] = { 255, 0,   0,   1.5f, 0.10f },
    [LED_RECORDING]     = { 0,   200, 255, 2.0f, 0.08f },
    [LED_IDENTIFY]      = { 255, 255, 255, 0.6f, 0.05f },
};

static volatile led_mode_t s_mode = LED_OFF;
static volatile bool s_paused;
static TaskHandle_t s_task;
static led_mode_t s_before_identify;
static esp_timer_handle_t s_ident_timer;

static void task(void *arg)
{
    led_mode_t cur = LED_OFF; int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (s_paused) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
        led_mode_t m = s_mode;
        if (m != cur) { cur = m; t0 = esp_timer_get_time(); }      /* restart the breath on a change: immediate feedback */
        const pattern_t *p = &PAT[cur];
        float t = (esp_timer_get_time() - t0) / 1e6f;
        float l = (1.0f - cosf(2.0f * (float)M_PI * t / p->period_s)) * 0.5f;   /* 0..1 raised cosine */
        l = l * l;                                                               /* perceptual: longer near-dark, softer peak */
        l = p->floor + (1.0f - p->floor) * l;
        if (cur == LED_OFF) l = 0;
        lp5810_set_rgb((uint8_t)(p->r * l + 0.5f), (uint8_t)(p->g * l + 0.5f), (uint8_t)(p->b * l + 0.5f));
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

esp_err_t led_start(void)
{
    if (!s_task) task_create_psram(task, "led", 2048, NULL, 3, &s_task, 0);
    return ESP_OK;
}

void led_set(led_mode_t mode) { s_mode = mode; }
led_mode_t led_get(void) { return s_mode; }
void led_pause(bool on) { s_paused = on; }

static void ident_done(void *arg) { if (s_mode == LED_IDENTIFY) s_mode = s_before_identify; }

void led_identify(uint32_t seconds)
{
    if (!s_ident_timer) { esp_timer_create_args_t a = { .callback = ident_done, .name = "led_ident" }; esp_timer_create(&a, &s_ident_timer); }
    if (s_mode != LED_IDENTIFY) s_before_identify = s_mode;
    s_mode = LED_IDENTIFY;
    esp_timer_stop(s_ident_timer);
    esp_timer_start_once(s_ident_timer, (uint64_t)(seconds ? seconds : 5) * 1000000ULL);
}
