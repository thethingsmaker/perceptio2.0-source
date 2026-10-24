/*
 * Core-1 IMU sampler: DRDY interrupt -> one burst read -> consumers.
 * The store (NAND log) and the live stream are sinks.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "imu.h"

typedef struct {
    uint32_t     rate_hz;        /* samples/s over the last second */
    uint32_t     total;          /* since start */
    float        avg_hz;         /* total / running time: the real ODR (927.x), stable unlike rate_hz */
    uint32_t     missed;         /* DRDY waits that timed out (IMU stopped?) */
    imu_sample_t last;
    float        hg_peak_g;      /* max |a_hg| over the last second */
} sampler_stats_t;

/* Fed for every sample, from the core-1 task. Must be lock-free and fast.
 * t_us = esp_timer time captured in the DRDY ISR (the sample's own timestamp, ~µs jitter). */
typedef void (*sampler_sink_fn)(const imu_sample_t *s, int64_t t_us);

esp_err_t sampler_start(void);
void      sampler_add_sink(sampler_sink_fn fn);      /* up to 4 */
void      sampler_get_stats(sampler_stats_t *out);
