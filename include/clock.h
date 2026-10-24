/*
 * Wall-clock time. The ESP32-S3 RTC runs across resets and deep sleep but is lost in ship mode
 * (no backup battery), so time is "unknown" after every cold start until NTP (internet) or the app
 * (POST /api/v1/time) sets it. Sessions timestamp with esp_timer (monotonic) + this, when known.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

typedef enum { CLK_SRC_NONE = 0, CLK_SRC_NTP, CLK_SRC_APP, CLK_SRC_RTC } clock_source_t;   /* RTC = survived a reset */

typedef struct {
    bool           valid;          /* year >= 2024 */
    clock_source_t source;
    time_t         now;            /* epoch, UTC */
    time_t         last_sync;      /* epoch of the last NTP/app set, 0 if never */
    uint32_t       age_s;          /* seconds since last sync */
    char           local[32];      /* "2026-10-22 14:38:05 CEST" */
    char           tz[48];
} clock_status_t;

esp_err_t clock_init(const char *posix_tz);       /* applies TZ; detects a surviving RTC time */
void      clock_set_tz(const char *posix_tz);     /* live */
void      clock_network_up(void);                 /* start/restart SNTP (called on got-IP) */
esp_err_t clock_set_epoch(time_t epoch, clock_source_t src);   /* from the app */
void      clock_get_status(clock_status_t *st);
const char *clock_source_name(clock_source_t s);

/* Backend-facing helpers (UTC, ms). */
int64_t   clock_utc_ms(void);                     /* 0 when the clock is not valid */
bool      clock_synced_since_boot(void);          /* an NTP sync completed this boot */
int32_t   clock_last_offset_ms(void);             /* correction applied by the last NTP sync (valid only if ..._known) */
bool      clock_last_offset_known(void);          /* false until a sync corrected an already-valid clock */
typedef void (*clock_sync_fn)(int32_t offset_ms, bool offset_known);
void      clock_set_sync_hook(clock_sync_fn fn);  /* called from the SNTP callback on every completed sync */
int64_t   clock_last_sync_utc_ms(void);
void      clock_resync(void);                     /* force an SNTP exchange now (no-op if offline) */
