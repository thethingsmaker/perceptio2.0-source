/* TI LP5810 4-channel LED driver, "D" variant (I2C base 0x5C, 10-bit register space: RA[9:8] in the address).
 * Board: FM-B2020RGBA-HG common anode, OUT0=R OUT1=G OUT2=B, OUT3 open. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define LP5810_ADDR_BASE   0x5C

esp_err_t lp5810_write(uint16_t reg, uint8_t v);
esp_err_t lp5810_read(uint16_t reg, uint8_t *v);

/* Direct-drive, manual PWM; returns ESP_FAIL if the chip reports a config error. */
esp_err_t lp5810_init(void);
/* 0..100 % — scales per-channel DC (G/B capped for Vf headroom at 3V3). */
esp_err_t lp5810_set_brightness(uint8_t pct);
/* PWM per channel, 0..255. */
esp_err_t lp5810_set_rgb(uint8_t r, uint8_t g, uint8_t b);
