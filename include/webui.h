/*
 * Perceptio 2.0 — HTTP/WebSocket API + embedded web app.
 * Runs on core 0 (the WiFi/UI core); must stay responsive whatever core 1 does.
 * The web page is just one client of /api/v1 — the mobile app uses the same API (see API.md).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "imu.h"
#include "clock.h"
#include "cloud.h"
#include "store.h"

typedef struct {
    uint32_t uptime_s;
    uint32_t heap_free, heap_min;
    float    vbat;
    bool     vbat_cal;
    int      soc_pct;
    uint8_t  bq_stat0, bq_stat1;
    int8_t   rssi;
    char     ip[16];
    /* active IMU configuration (what the stream is scaled with) */
    int      odr_hz, xl_fs_g, gy_fs_dps, hg_fs_g;
    float    xl_mg_lsb, gy_mdps_lsb, hg_mg_lsb;   /* sensitivities (ST driver values; gyro is NOT fs/32768) */
    /* core-1 sampler */
    uint32_t imu_rate_hz, imu_total, imu_missed;
    int16_t  ax, ay, az, gx, gy, gz;
    int32_t  hx, hy, hz;           /* high-g, mg */
    float    hg_peak_g;            /* max |a_hg| over the last second */
    float    imu_temp_c;
    int      tests_pass, tests_total;
    clock_status_t clk;
    cloud_status_t cloud;
    store_status_t store;
} webui_status_t;

typedef void (*webui_status_fn)(webui_status_t *out);

typedef imu_sample_t webui_sample_t;

/* Sampler sink: called from the core-1 task for every sample. Lock-free, never blocks. */
void webui_feed(const imu_sample_t *smp, int64_t t_us);

/* Live-stream flush rate (frames/s to browsers), 10..60. */
void webui_set_stream_hz(int hz);

/* Start the server pinned to core 0. `fn` fills /api/v1/status. */
esp_err_t webui_start(webui_status_fn fn);
