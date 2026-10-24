#include <string.h>
#include "imu.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "IMU";
static spi_device_handle_t s_dev;
static imu_cfg_t s_cfg;
static stmdev_ctx_t s_ctx;

/* ---- platform glue for the ST driver (polling SPI: no ISR, stays on the calling core) ---- */
static int32_t plat_write(void *h, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    uint8_t tx[1 + 32];
    if (len > 32) return -1;
    tx[0] = reg & 0x7F; memcpy(tx + 1, buf, len);
    spi_transaction_t t = { .length = 8 * (1 + len), .tx_buffer = tx };
    return spi_device_polling_transmit(s_dev, &t) == ESP_OK ? 0 : -1;
}
static int32_t plat_read(void *h, uint8_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t tx[1 + 32] = { reg | 0x80 }, rx[1 + 32];
    if (len > 32) return -1;
    spi_transaction_t t = { .length = 8 * (1 + len), .tx_buffer = tx, .rx_buffer = rx };
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) return -1;
    memcpy(buf, rx + 1, len);
    return 0;
}
static void plat_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

esp_err_t imu_read_reg(uint8_t reg, uint8_t *buf, uint16_t len) { return plat_read(NULL, reg, buf, len) ? ESP_FAIL : ESP_OK; }
esp_err_t imu_write_reg(uint8_t reg, uint8_t v) { return plat_write(NULL, reg, &v, 1) ? ESP_FAIL : ESP_OK; }
const stmdev_ctx_t *imu_ctx(void) { return &s_ctx; }

/* ---- */
esp_err_t imu_init(void)
{
    if (!s_dev) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 10 * 1000 * 1000, .mode = 0, .spics_io_num = PIN_IMU_CS,
            .queue_size = 4, .input_delay_ns = 25,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_dev));
        s_ctx.write_reg = plat_write; s_ctx.read_reg = plat_read; s_ctx.mdelay = plat_delay;
    }
    /* first transaction on the bus: kill the I2C/I3C engine so bus noise can never address it */
    lsm6dsv320x_ui_i2c_i3c_mode_set(&s_ctx, LSM6DSV320X_I2C_I3C_DISABLE);
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t who = 0;
    lsm6dsv320x_device_id_get(&s_ctx, &who);
    if (who != IMU_WHO_AM_I) { ESP_LOGE(TAG, "WHO_AM_I 0x%02X (expect 0x%02X)", who, IMU_WHO_AM_I); return ESP_ERR_NOT_FOUND; }
    ESP_LOGI(TAG, "LSM6DSV320X present");
    return ESP_OK;
}

static lsm6dsv320x_data_rate_t odr_enum(int hz)
{
    switch (hz) { case 480: return LSM6DSV320X_ODR_AT_480Hz; case 1920: return LSM6DSV320X_ODR_AT_1920Hz; default: return LSM6DSV320X_ODR_AT_960Hz; }
}
static lsm6dsv320x_hg_xl_data_rate_t hg_odr_enum(int hz)
{
    switch (hz) { case 480: return LSM6DSV320X_HG_XL_ODR_AT_480Hz; case 1920: return LSM6DSV320X_HG_XL_ODR_AT_1920Hz; default: return LSM6DSV320X_HG_XL_ODR_AT_960Hz; }
}

esp_err_t imu_configure(const imu_cfg_t *cfg)
{
    s_cfg = *cfg;
    int32_t r = 0;
    r |= lsm6dsv320x_sw_reset(&s_ctx);
    r |= lsm6dsv320x_ui_i2c_i3c_mode_set(&s_ctx, LSM6DSV320X_I2C_I3C_DISABLE);   /* re-assert after reset */
    r |= lsm6dsv320x_auto_increment_set(&s_ctx, 1);
    r |= lsm6dsv320x_block_data_update_set(&s_ctx, 1);
    r |= lsm6dsv320x_data_ready_mode_set(&s_ctx, LSM6DSV320X_DRDY_PULSED);

    lsm6dsv320x_gy_full_scale_t gfs = cfg->gy_fs_dps <= 250 ? LSM6DSV320X_250dps : cfg->gy_fs_dps <= 500 ? LSM6DSV320X_500dps :
                                      cfg->gy_fs_dps <= 1000 ? LSM6DSV320X_1000dps : cfg->gy_fs_dps <= 2000 ? LSM6DSV320X_2000dps : LSM6DSV320X_4000dps;
    lsm6dsv320x_xl_full_scale_t xfs = cfg->xl_fs_g <= 2 ? LSM6DSV320X_2g : cfg->xl_fs_g <= 4 ? LSM6DSV320X_4g : cfg->xl_fs_g <= 8 ? LSM6DSV320X_8g : LSM6DSV320X_16g;
    lsm6dsv320x_hg_xl_full_scale_t hfs = cfg->hg_fs_g <= 32 ? LSM6DSV320X_32g : cfg->hg_fs_g <= 64 ? LSM6DSV320X_64g :
                                         cfg->hg_fs_g <= 128 ? LSM6DSV320X_128g : cfg->hg_fs_g <= 256 ? LSM6DSV320X_256g : LSM6DSV320X_320g;
    r |= lsm6dsv320x_gy_full_scale_set(&s_ctx, gfs);
    r |= lsm6dsv320x_xl_full_scale_set(&s_ctx, xfs);
    r |= lsm6dsv320x_hg_xl_full_scale_set(&s_ctx, hfs);

    /* low-g XL and gyro must be in high-performance mode for the HG channel to run (driver rule) */
    r |= lsm6dsv320x_xl_setup(&s_ctx, odr_enum(cfg->odr_hz), LSM6DSV320X_XL_HIGH_PERFORMANCE_MD);
    r |= lsm6dsv320x_gy_setup(&s_ctx, odr_enum(cfg->odr_hz), LSM6DSV320X_GY_HIGH_PERFORMANCE_MD);
    r |= lsm6dsv320x_hg_xl_data_rate_set(&s_ctx, hg_odr_enum(cfg->odr_hz), 1 /* regout_en */);

    vTaskDelay(pdMS_TO_TICKS(100));            /* gyro turn-on 40 ms + filter settling */
    uint8_t c1 = 0, c2 = 0, hg = 0;
    imu_read_reg(LSM6DSV320X_CTRL1, &c1, 1); imu_read_reg(LSM6DSV320X_CTRL2, &c2, 1); imu_read_reg(LSM6DSV320X_CTRL1_XL_HG, &hg, 1);
    ESP_LOGI(TAG, "cfg %d Hz, ±%d g, ±%d dps, HG ±%d g  (CTRL1 %02X CTRL2 %02X CTRL1_XL_HG %02X) %s",
             cfg->odr_hz, cfg->xl_fs_g, cfg->gy_fs_dps, cfg->hg_fs_g, c1, c2, hg, r ? "ERRORS" : "ok");
    return r ? ESP_FAIL : ESP_OK;
}

const imu_cfg_t *imu_get_cfg(void) { return &s_cfg; }

imu_scale_t imu_get_scale(void)
{
    imu_scale_t s;
    s.xl_mg_lsb   = s_cfg.xl_fs_g * 1000.0f / 32768.0f;               /* 0.488 at ±16 g */
    s.gy_mdps_lsb = s_cfg.gy_fs_dps * 0.035f;                         /* ST driver: 8.75 @250 … 140 @4000 mdps/LSB — NOT fs/32768 */
    switch (s_cfg.hg_fs_g) {                                          /* from the ST driver, not fs/32768 */
    case 32:  s.hg_mg_lsb = 0.976f; break;
    case 64:  s.hg_mg_lsb = 1.952f; break;
    case 128: s.hg_mg_lsb = 3.904f; break;
    case 256: s.hg_mg_lsb = 7.808f; break;
    default:  s.hg_mg_lsb = 10.417f; break;
    }
    return s;
}

esp_err_t imu_drdy_int1(bool en)
{
    lsm6dsv320x_pin_int_route_t route = { 0 };
    route.drdy_xl = en;
    return lsm6dsv320x_pin_int1_route_set(&s_ctx, &route) ? ESP_FAIL : ESP_OK;
}

esp_err_t imu_read_sample(imu_sample_t *s)
{
    uint8_t raw[26];
    esp_err_t e = imu_read_reg(LSM6DSV320X_OUT_TEMP_L, raw, 26);      /* 0x20..0x39 */
    for (int i = 0; i < 7; i++) s->v[i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
    for (int i = 0; i < 3; i++) s->v[7 + i] = (int16_t)(raw[20 + 2 * i] | (raw[21 + 2 * i] << 8));
    return e;
}
