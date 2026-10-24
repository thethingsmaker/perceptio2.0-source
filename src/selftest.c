#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>
#include "selftest.h"
#include "board.h"
#include "bq25180.h"
#include "lp5810.h"
#include "led.h"
#include "imu.h"
#include "w25n01kv.h"
#include "power.h"
#include "net.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "SELFTEST";
static selftest_result_t s_res[40];
static int s_n;

static void record(const char *name, bool pass, const char *fmt, ...)
{
    if (s_n >= (int)(sizeof s_res / sizeof s_res[0])) return;
    selftest_result_t *r = &s_res[s_n++];
    r->name = name; r->pass = pass;
    va_list ap; va_start(ap, fmt); vsnprintf(r->info, sizeof r->info, fmt, ap); va_end(ap);
    ESP_LOGI(TAG, "[%s] %s — %s", pass ? "PASS" : "FAIL", name, r->info);
}

/* ------------------------------------------------------------------ */
static void t_soc(void)
{
    esp_chip_info_t ci; esp_chip_info(&ci);
    uint32_t flash = 0; esp_flash_get_size(NULL, &flash);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "ESP32-S3 rev %d.%d, flash %" PRIu32 " MB, PSRAM %u KB, free heap %u KB", ci.revision / 100, ci.revision % 100, flash >> 20, (unsigned)(psram >> 10), (unsigned)(heap >> 10));
    record("SoC/flash/PSRAM", flash == 4u << 20 && psram >= 2000 * 1024, "flash %" PRIu32 "MB psram %uKB (expect 4MB / 2048KB)", flash >> 20, (unsigned)(psram >> 10));
}

/* ------------------------------------------------------------------ */
static esp_err_t i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c); i2c_master_write_byte(c, (addr << 1) | I2C_MASTER_WRITE, true); i2c_master_stop(c);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, c, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(c);
    return r;
}

static void t_i2c(void)
{
    char found[64] = "";
    for (uint8_t a = 0x08; a < 0x78; a++)
        if (i2c_probe(a) == ESP_OK) { char t[8]; snprintf(t, sizeof t, "%02X ", a); strlcat(found, t, sizeof found); }
    bool bq = strstr(found, "6A ") != NULL, lp = strstr(found, "5C ") != NULL;
    record("I2C scan", bq && lp, "[%s] expect 5C-5F,6C-6F (LP) + 6A (BQ)", found);

    uint8_t id = 0xFF, ichg = 0; bq25180_status_t st;
    bq25180_read(BQ25180_MASK_ID, &id); bq25180_read(BQ25180_ICHG_CTRL, &ichg); bq25180_get_status(&st);
    int code = ichg & 0x7F, ma = code <= 30 ? code + 5 : 40 + (code - 31) * 10;    /* decode ICHGCODE */
    record("BQ25180", (id & 0x0F) == 0 && ma == g_cfg.ichg_ma && !(ichg & 0x80),
           "id %X ICHG %d mA (cfg %ld) VIN %d %s", id & 0xF, ma, (long)g_cfg.ichg_ma, st.vin_pgood, bq25180_chg_name(st.chg));

    uint8_t err = 0xFF; lp5810_read(0x300, &err);
    const uint8_t seq[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255} };
    led_pause(true);
    for (int i = 0; i < 4; i++) { lp5810_set_rgb(seq[i][0], seq[i][1], seq[i][2]); vTaskDelay(pdMS_TO_TICKS(400)); }
    lp5810_set_rgb(0, 0, 0);
    led_pause(false);
    record("LP5810 LED", err == 0, "cfg_err %02X — R, G, B, white shown", err);
}

/* ------------------------------------------------------------------ */
static volatile uint32_t s_edges;
static void IRAM_ATTR edge_isr(void *arg) { s_edges++; }

static void t_imu(void)
{
    uint8_t who = 0; imu_read_reg(LSM6DSV320X_WHO_AM_I, &who, 1);
    record("IMU WHO_AM_I", who == IMU_WHO_AM_I, "0x%02X", who);
    if (who != IMU_WHO_AM_I) return;

    uint8_t hg = 0; imu_read_reg(LSM6DSV320X_CTRL1_XL_HG, &hg, 1);
    record("IMU high-g enable", (hg & 0x80) && (hg & 0x07) != 0, "CTRL1_XL_HG %02X (regout_en, ODR, FS)", hg);

    imu_scale_t sc = imu_get_scale();
    imu_sample_t s;
    for (int i = 0; i < 5; i++) {
        imu_read_sample(&s);
        ESP_LOGI(TAG, "T=%.1fC A[mg]=%6.0f %6.0f %6.0f G[dps]=%7.1f %7.1f %7.1f HG[mg]=%6.0f %6.0f %6.0f",
                 25.0f + s.v[0] / 256.0f, s.v[4] * sc.xl_mg_lsb, s.v[5] * sc.xl_mg_lsb, s.v[6] * sc.xl_mg_lsb,
                 s.v[1] * sc.gy_mdps_lsb / 1000, s.v[2] * sc.gy_mdps_lsb / 1000, s.v[3] * sc.gy_mdps_lsb / 1000,
                 s.v[7] * sc.hg_mg_lsb, s.v[8] * sc.hg_mg_lsb, s.v[9] * sc.hg_mg_lsb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* Motion-independent checks: the device is always handled while it boots, so nothing here assumes it is still.
     * 50 samples over 0.5 s. A live sensor's raw output always moves by at least a few LSB (noise, even on a table);
     * a dead or stuck one repeats the same value or sits pinned at full scale. Offsets are not judged here. */
    int16_t lo[10] = {0}, hi[10] = {0};
    float asum = 0;
    for (int i = 0; i < 50; i++) {
        imu_read_sample(&s);
        for (int k = 1; k < 10; k++) {
            if (i == 0 || s.v[k] < lo[k]) lo[k] = s.v[k];
            if (i == 0 || s.v[k] > hi[k]) hi[k] = s.v[k];
        }
        asum += sqrtf((float)s.v[4] * s.v[4] + (float)s.v[5] * s.v[5] + (float)s.v[6] * s.v[6]) * sc.xl_mg_lsb / 1000;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool g_live = true, g_pinned = false, a_live = true;
    for (int k = 1; k <= 3; k++) {
        if (hi[k] == lo[k]) g_live = false;                       /* frozen axis */
        if (lo[k] >= 32760 || hi[k] <= -32760) g_pinned = true;   /* every sample at full scale */
    }
    for (int k = 4; k <= 6; k++) if (hi[k] == lo[k]) a_live = false;
    float a = asum / 50;
    record("IMU accel live", a_live && a > 0.2f && a < 16.0f, "mean |a| = %.2f g over 0.5 s, raw spread %d/%d/%d LSB",
           a, hi[4] - lo[4], hi[5] - lo[5], hi[6] - lo[6]);
    /* ±320 g channel vs low-g channel on the same sample: both see the same acceleration, so the difference is the
     * high-g offset whatever the motion (spec ~±1-2 g; valid while the low-g channel is below its 16 g full scale). */
    float d[3], dmax = 0;
    for (int k = 0; k < 3; k++) { d[k] = (s.v[7 + k] * sc.hg_mg_lsb - s.v[4 + k] * sc.xl_mg_lsb) / 1000; if (fabsf(d[k]) > dmax) dmax = fabsf(d[k]); }
    record("IMU high-g offset", dmax < 3.0f, "high-g minus low-g per axis: %+.2f %+.2f %+.2f g (expect within ±3)", d[0], d[1], d[2]);
    record("IMU gyro live", g_live && !g_pinned, "raw spread %d/%d/%d LSB over 0.5 s%s",
           hi[1] - lo[1], hi[2] - lo[2], hi[3] - lo[3], g_pinned ? " — an axis is pinned at full scale" : g_live ? "" : " — an axis never changes");

    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_IMU_INT1, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE };
    gpio_config(&io);
    gpio_isr_handler_add(PIN_IMU_INT1, edge_isr, NULL);
    imu_drdy_int1(true);
    s_edges = 0; vTaskDelay(pdMS_TO_TICKS(1000)); uint32_t rate = s_edges;
    imu_drdy_int1(false);
    gpio_isr_handler_remove(PIN_IMU_INT1);
    int odr = imu_get_cfg()->odr_hz;
    record("IMU INT1 -> IO5", rate > odr * 0.93f && rate < odr * 1.07f, "%" PRIu32 " DRDY/s (ODR %d)", rate, odr);
    record("IMU INT2 -> IO17 idle", gpio_get_level(PIN_IMU_INT2) == 0, "level %d", gpio_get_level(PIN_IMU_INT2));
}

/* ------------------------------------------------------------------ */
static void fill_pattern(uint8_t *dst, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 12345;
    for (int i = 0; i < NAND_PAGE_SIZE; i++) { x = x * 1664525u + 1013904223u; dst[i] = x >> 24; }
}

static void t_nand(void)
{
    const nand_info_t *ni = nand_info();
    record("NAND JEDEC ID", ni->jedec[0] == 0xEF && ni->jedec[1] == 0xAE && ni->jedec[2] == 0x21, "%02X %02X %02X", ni->jedec[0], ni->jedec[1], ni->jedec[2]);
    record("NAND unlock", (ni->sr1 & 0x7C) == 0, "SR1 %02X SR2 %02X", ni->sr1, ni->sr2);
    record("NAND bad-block scan", ni->bad_blocks <= 20, "%d factory bad of %d", ni->bad_blocks, NAND_BLOCKS);

    static uint8_t WORD_ALIGNED_ATTR w[NAND_PAGE_SIZE], r[NAND_PAGE_SIZE];
    /* reserved region only (blocks 0..7, 1020..1023): 8..1019 hold recordings (store.h) */
    uint16_t blk = 1020; while (nand_block_is_bad(blk) && blk < NAND_BLOCKS - 1) blk++;
    uint32_t pg = blk * NAND_PAGES_PER_BLK;
    esp_err_t e = nand_block_erase(blk);
    record("NAND block erase", e == ESP_OK, "block %u: %s", blk, esp_err_to_name(e));
    fill_pattern(w, pg);
    e = nand_page_program(pg, w, NAND_PAGE_SIZE);
    record("NAND page program", e == ESP_OK, "page %" PRIu32 ": %s", pg, esp_err_to_name(e));
    int ecc = 0; e = nand_page_read(pg, 0, r, NAND_PAGE_SIZE, &ecc);
    int mism = 0; for (int i = 0; i < NAND_PAGE_SIZE; i++) if (r[i] != w[i]) mism++;
    record("NAND read verify", e == ESP_OK && mism == 0, "%d mismatches, ECC %d", mism, ecc);
    nand_block_erase(blk);
}

static void t_nand_stress(void)
{
    static uint8_t WORD_ALIGNED_ATTR w[NAND_PAGE_SIZE], r[NAND_PAGE_SIZE];
    uint16_t blks[16]; int n = 0;
    for (int b = 0; b < NAND_BLOCKS && n < 16; b++)                 /* reserved blocks only, never the recording area */
        if ((b < 8 || b >= 1020) && !nand_block_is_bad(b)) blks[n++] = b;

    int64_t t0 = esp_timer_get_time(); bool ok = true;
    for (int i = 0; i < n; i++) ok &= nand_block_erase(blks[i]) == ESP_OK;
    record("NAND stress erase", ok, "%d blocks in %lld ms", n, (esp_timer_get_time() - t0) / 1000);

    t0 = esp_timer_get_time(); ok = true; int pages = 0;
    for (int i = 0; i < n && ok; i++)
        for (int p = 0; p < NAND_PAGES_PER_BLK && ok; p++) {
            uint32_t pg = blks[i] * NAND_PAGES_PER_BLK + p;
            fill_pattern(w, pg); ok = nand_page_program(pg, w, NAND_PAGE_SIZE) == ESP_OK; pages++;
        }
    int64_t dt = esp_timer_get_time() - t0;
    record("NAND stress program", ok, "%d pages (%d KB) %lld KB/s", pages, pages * 2, (int64_t)pages * NAND_PAGE_SIZE * 1000000 / dt / 1024);

    t0 = esp_timer_get_time(); uint32_t mism = 0; int ecc_max = 0;
    for (int i = 0; i < n; i++)
        for (int p = 0; p < NAND_PAGES_PER_BLK; p++) {
            uint32_t pg = blks[i] * NAND_PAGES_PER_BLK + p; int ecc = 0;
            nand_page_read(pg, 0, r, NAND_PAGE_SIZE, &ecc); if (ecc > ecc_max) ecc_max = ecc;
            fill_pattern(w, pg);
            for (int k = 0; k < NAND_PAGE_SIZE; k++) if (r[k] != w[k]) mism++;
        }
    dt = esp_timer_get_time() - t0;
    record("NAND stress verify", mism == 0 && ecc_max <= 1, "%" PRIu32 " mismatches, ECC max %d, %lld KB/s @%lu MHz",
           mism, ecc_max, (int64_t)pages * NAND_PAGE_SIZE * 1000000 / dt / 1024, (unsigned long)nand_info()->clock_hz / 1000000);
    for (int i = 0; i < n; i++) nand_block_erase(blks[i]);
}

/* ------------------------------------------------------------------ */
static void t_power(uint32_t button_wait_s)
{
    float v = power_vbat();
    record("VBAT sense", v > 3.0f && v < 4.4f, "%.2f V (%s)", v, power_vbat_calibrated() ? "calibrated" : "uncalibrated");
    if (!button_wait_s) return;
    uint32_t e0 = power_int_edges(); uint8_t w = 0;
    bq25180_take_wake_flags();
    for (uint32_t i = 0; i < button_wait_s; i++) {
        ESP_LOGW(TAG, ">>> PRESS THE BUTTON once (short press) — %" PRIu32 " s left <<<", button_wait_s - i);
        vTaskDelay(pdMS_TO_TICKS(1000));
        w |= bq25180_take_wake_flags();
        if (w) break;
    }
    record("Button -> BQ /INT -> IO6", w != 0, "WAKE flags %02X, %" PRIu32 " /INT edges", w, power_int_edges() - e0);
}

static void t_wifi(void)
{
    int n = net_scan();
    record("WiFi scan", n > 0, "%d networks (antenna/RF path)", n);
}

/* ------------------------------------------------------------------ */
void selftest_run(uint32_t g, uint32_t button_wait_s)
{
    s_n = 0;
    if (g & ST_SOC)   t_soc();
    if (g & ST_I2C)   t_i2c();
    if (g & ST_IMU)   t_imu();
    if (g & ST_NAND)  t_nand();
    if (g & ST_NAND_STRESS) t_nand_stress();
    if (g & ST_POWER) t_power(button_wait_s);
    if (g & ST_WIFI)  t_wifi();
    ESP_LOGI(TAG, "==== SELFTEST %d/%d passed ====", selftest_passed(), s_n);
    for (int i = 0; i < s_n; i++) ESP_LOGI(TAG, "  %-26s %s  %s", s_res[i].name, s_res[i].pass ? "PASS" : "FAIL", s_res[i].info);
}

int selftest_count(void) { return s_n; }
const selftest_result_t *selftest_result(int i) { return (i >= 0 && i < s_n) ? &s_res[i] : NULL; }
int selftest_passed(void) { int p = 0; for (int i = 0; i < s_n; i++) p += s_res[i].pass; return p; }
