#include "lp5810.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "LP5810";

#define R_CHIP_EN     0x000
#define R_DEV_CONFIG0 0x001
#define R_DEV_CONFIG1 0x002
#define R_DEV_CONFIG12 0x00D
#define R_CMD_UPDATE  0x010
#define R_LED_EN      0x020
#define R_DC0         0x030
#define R_PWM0        0x040
#define R_CONFIG_ERR  0x300

#define DC_R_MAX  0x40     /* 6.4 mA */
#define DC_GB_MAX 0x1E     /* 3.0 mA — green/blue Vf ~3 V leaves little headroom at 3V3 */

esp_err_t lp5810_write(uint16_t reg, uint8_t v)
{
    uint8_t b[2] = { reg & 0xFF, v };
    return i2c_master_write_to_device(I2C_PORT, LP5810_ADDR_BASE | ((reg >> 8) & 3), b, 2, pdMS_TO_TICKS(200));
}

esp_err_t lp5810_read(uint16_t reg, uint8_t *v)
{
    uint8_t r = reg & 0xFF;
    return i2c_master_write_read_device(I2C_PORT, LP5810_ADDR_BASE | ((reg >> 8) & 3), &r, 1, v, 1, pdMS_TO_TICKS(200));
}

esp_err_t lp5810_init(void)
{
    esp_err_t e = lp5810_write(R_CHIP_EN, 0x01);
    if (e != ESP_OK) { ESP_LOGE(TAG, "not responding (%s)", esp_err_to_name(e)); return e; }
    vTaskDelay(pdMS_TO_TICKS(2));
    e |= lp5810_write(R_DEV_CONFIG0, 0x00);     /* max current 25.5 mA range */
    e |= lp5810_write(R_DEV_CONFIG1, 0x00);     /* 4-channel direct drive, manual PWM */
    e |= lp5810_write(R_DEV_CONFIG12, 0x0B);    /* LSD threshold per datasheet */
    e |= lp5810_write(R_CMD_UPDATE, 0x55);
    uint8_t err = 0xFF; lp5810_read(R_CONFIG_ERR, &err);
    e |= lp5810_write(R_LED_EN, 0x07);          /* OUT0..2 */
    e |= lp5810_set_brightness(100);
    e |= lp5810_set_rgb(0, 0, 0);
    if (err) { ESP_LOGE(TAG, "config error 0x%02X", err); return ESP_FAIL; }
    return e;
}

esp_err_t lp5810_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    esp_err_t e = lp5810_write(R_DC0 + 0, DC_R_MAX * pct / 100);
    e |= lp5810_write(R_DC0 + 1, DC_GB_MAX * pct / 100);
    e |= lp5810_write(R_DC0 + 2, DC_GB_MAX * pct / 100);
    return e;
}

esp_err_t lp5810_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t e = lp5810_write(R_PWM0 + 0, r);
    e |= lp5810_write(R_PWM0 + 1, g);
    e |= lp5810_write(R_PWM0 + 2, b);
    return e;
}
