/*
 * Perceptio 2.0 — board definition (hw rev 20) and bus bring-up.
 * Pin map verified against Netlist_Schematic2_2026-09-23 and on hardware 2026-10-21.
 */
#pragma once
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"

#ifndef PERCEPTIO_HW_REV
#define PERCEPTIO_HW_REV 20
#endif

/* I2C0: BQ25180 charger + LP5810 LED driver (4.7 k pull-ups on board) */
#define PIN_I2C_SCL     37
#define PIN_I2C_SDA     36
#define I2C_PORT        I2C_NUM_0
#define I2C_FREQ_HZ     400000

/* SPI3 (GPIO matrix): IMU only */
#define PIN_SPI3_SCK    18
#define PIN_SPI3_MOSI   21
#define PIN_SPI3_MISO   33
#define PIN_IMU_CS      15      /* R55 10k pull-up */
#define PIN_IMU_INT1    5
#define PIN_IMU_INT2    17

/* SPI2 (IO_MUX pins, up to 80 MHz): NAND only */
#define PIN_SPI2_CLK    12
#define PIN_SPI2_MOSI   11
#define PIN_SPI2_MISO   13
#define PIN_NAND_CS     10

/* Power */
#define PIN_BQ_INT      6       /* BQ25180 /INT, R34 10k pull-up; button events arrive here */
#define PIN_VBAT_SENS   4       /* 1 M / 1 M divider -> ADC1_CH3 */
#define VBAT_ADC_CH     ADC_CHANNEL_3
#define VBAT_DIV_RATIO  2.0f

/* Reserved / strapping: IO0 (R56 up), IO3 (R54 down), IO26 (PSRAM), IO45, IO46, IO19/IO20 USB */

/* Initialise I2C0, SPI2 and SPI3 buses (not the devices on them). Idempotent. */
esp_err_t board_init(void);
