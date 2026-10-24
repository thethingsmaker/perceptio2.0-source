/*
 * LSM6DSV320X on SPI3 — thin wrapper over the ST platform-independent driver
 * (components/lsm6dsv320x). Configuration goes through the ST API; the hot path
 * (one burst read per data-ready) is a direct polling SPI transfer.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lsm6dsv320x_reg.h"

#define IMU_WHO_AM_I      0x73

typedef struct {
    int      odr_hz;        /* 480 / 960 / 1920 — all three channels share it (HG ladder has no 800) */
    int      xl_fs_g;       /* 2 / 4 / 8 / 16 */
    int      gy_fs_dps;     /* 250 / 500 / 1000 / 2000 / 4000 */
    int      hg_fs_g;       /* 32 / 64 / 128 / 256 / 320 */
} imu_cfg_t;

/* One sample, raw LSB: TEMP, GX, GY, GZ, AX, AY, AZ, HGX, HGY, HGZ */
typedef struct { int16_t v[10]; } imu_sample_t;
#define IMU_SAMPLE_BYTES  20

/* Sensitivities for the active config */
typedef struct { float xl_mg_lsb, gy_mdps_lsb, hg_mg_lsb; } imu_scale_t;

/* Add the SPI device, disable the I2C/I3C engine, check WHO_AM_I. */
esp_err_t imu_init(void);
/* SW reset + apply configuration (also sets BDU, address auto-increment, DRDY pulsed). */
esp_err_t imu_configure(const imu_cfg_t *cfg);
const imu_cfg_t   *imu_get_cfg(void);
imu_scale_t        imu_get_scale(void);
/* Route low-g accelerometer data-ready to INT1 (pulsed) or disable it. */
esp_err_t imu_drdy_int1(bool en);
/* Burst read 0x20..0x39 (26 bytes) and unpack. ~22 us at 10 MHz. Safe from any core. */
esp_err_t imu_read_sample(imu_sample_t *s);
/* Raw access (also used by selftest) */
esp_err_t imu_read_reg(uint8_t reg, uint8_t *buf, uint16_t len);
esp_err_t imu_write_reg(uint8_t reg, uint8_t v);
const stmdev_ctx_t *imu_ctx(void);
