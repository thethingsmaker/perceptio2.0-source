#include <math.h>
#include <string.h>
#include "sampler.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "SAMPLER";
static SemaphoreHandle_t s_drdy;
static sampler_sink_fn s_sinks[4];
static int s_nsinks;
static volatile sampler_stats_t s_stats;
static TaskHandle_t s_task;

static volatile int64_t s_drdy_us;

static void IRAM_ATTR drdy_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    s_drdy_us = esp_timer_get_time();                       /* sample timestamp, taken at the data-ready edge */
    xSemaphoreGiveFromISR(s_drdy, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_IMU_INT1, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE };
    gpio_config(&io);
    gpio_isr_handler_add(PIN_IMU_INT1, drdy_isr, NULL);     /* ISR service is on core 1 (board_init) */
    imu_drdy_int1(true);
    ESP_LOGI(TAG, "running on core %d, prio %u", xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    imu_scale_t sc = imu_get_scale();
    uint32_t window = 0; int64_t t0 = esp_timer_get_time(), t_start = 0; float peak2 = 0;
    imu_sample_t s;
    for (;;) {
        if (xSemaphoreTake(s_drdy, pdMS_TO_TICKS(100)) != pdTRUE) { s_stats.missed++; continue; }
        int64_t t_us = s_drdy_us;
        imu_read_sample(&s);
        for (int i = 0; i < s_nsinks; i++) s_sinks[i](&s, t_us);
        memcpy((void *)&s_stats.last, &s, sizeof s);
        float hx = s.v[7] * sc.hg_mg_lsb, hy = s.v[8] * sc.hg_mg_lsb, hz = s.v[9] * sc.hg_mg_lsb;
        float m2 = hx * hx + hy * hy + hz * hz;
        if (m2 > peak2) peak2 = m2;
        if (!t_start) t_start = t_us;
        s_stats.total++; window++;
        if (t_us > t_start + 2000000) s_stats.avg_hz = (float)(s_stats.total - 1) * 1e6f / (float)(t_us - t_start);
        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) { s_stats.rate_hz = window; window = 0; t0 = now; s_stats.hg_peak_g = sqrtf(peak2) / 1000.0f; peak2 = 0; }
    }
}

esp_err_t sampler_start(void)
{
    if (s_task) return ESP_OK;
    s_drdy = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(task, "imu_sampler", 4096, NULL, configMAX_PRIORITIES - 2, &s_task, 1);
    return ESP_OK;
}

void sampler_add_sink(sampler_sink_fn fn) { if (s_nsinks < 4) s_sinks[s_nsinks++] = fn; }
void sampler_get_stats(sampler_stats_t *out) { memcpy(out, (void *)&s_stats, sizeof *out); }
