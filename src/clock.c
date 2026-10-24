#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "clock.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "CLOCK";
static clock_source_t s_src = CLK_SRC_NONE;
static time_t s_last_sync;
static bool s_sntp_started, s_synced_boot;
static int32_t s_last_offset_ms;
static bool s_offset_known;
static clock_sync_fn s_sync_hook;
static char s_tz[48] = "UTC0";
static int64_t s_pre_wall_us, s_pre_mono_us;      /* snapshot before a sync, to measure the correction */

static bool time_valid(time_t t) { return t > 1704067200; }    /* 2024-01-01 */

static void snapshot(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    s_pre_wall_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    s_pre_mono_us = esp_timer_get_time();
}

static void on_sntp_sync(struct timeval *tv)
{
    /* offset = new wall time - (old wall time + elapsed). Only meaningful if the old wall time was valid. */
    bool was_valid = time_valid(s_pre_wall_us / 1000000);
    if (s_pre_mono_us && was_valid) {
        int64_t expected = s_pre_wall_us + (esp_timer_get_time() - s_pre_mono_us);
        int64_t now_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
        int64_t off = (now_us - expected) / 1000;
        if (off > INT32_MAX) off = INT32_MAX;
        if (off < INT32_MIN) off = INT32_MIN;
        s_last_offset_ms = (int32_t)off; s_offset_known = true;
    } else {
        s_last_offset_ms = 0; s_offset_known = false;        /* cold start: the clock was unset, no correction to report */
    }
    snapshot();
    s_src = CLK_SRC_NTP; s_synced_boot = true;
    s_last_sync = tv->tv_sec;
    char buf[32]; struct tm tm; localtime_r(&tv->tv_sec, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z", &tm);
    if (s_offset_known) ESP_LOGI(TAG, "NTP sync: %s (corrected %+ld ms)", buf, (long)s_last_offset_ms);
    else                ESP_LOGI(TAG, "NTP sync: %s (first sync, offset unknown)", buf);
    if (s_sync_hook) s_sync_hook(s_last_offset_ms, s_offset_known);
}

void clock_set_tz(const char *posix_tz)
{
    if (!posix_tz || !*posix_tz) posix_tz = "UTC0";
    strncpy(s_tz, posix_tz, sizeof s_tz - 1);
    setenv("TZ", s_tz, 1);
    tzset();
}

esp_err_t clock_init(const char *posix_tz)
{
    clock_set_tz(posix_tz);
    time_t now = time(NULL);
    if (time_valid(now)) {                      /* RTC kept running across a reset / deep sleep */
        s_src = CLK_SRC_RTC;
        ESP_LOGI(TAG, "time survived reset (RTC), will refresh via NTP");
    } else {
        ESP_LOGW(TAG, "time unknown (cold start) — waiting for NTP or the app");
    }
    return ESP_OK;
}

void clock_network_up(void)
{
    snapshot();
    if (s_sntp_started) { esp_netif_sntp_start(); return; }     /* re-sync on reconnect */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");   /* CONFIG_SNTP_MAX_SERVERS defaults to 1 */
    cfg.start = true;
    cfg.sync_cb = on_sntp_sync;
    cfg.smooth_sync = false;                    /* step the clock: sessions want absolute correctness, not smoothness */
    esp_err_t e = esp_netif_sntp_init(&cfg);
    s_sntp_started = e == ESP_OK;
    ESP_LOGI(TAG, "SNTP %s", s_sntp_started ? "started (pool.ntp.org)" : esp_err_to_name(e));
}

esp_err_t clock_set_epoch(time_t epoch, clock_source_t src)
{
    if (!time_valid(epoch)) return ESP_ERR_INVALID_ARG;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_src = src; s_last_sync = epoch;
    ESP_LOGI(TAG, "time set by %s", clock_source_name(src));
    return ESP_OK;
}

void clock_get_status(clock_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->now = time(NULL);
    st->valid = time_valid(st->now);
    st->source = s_src;
    st->last_sync = s_last_sync;
    st->age_s = (s_last_sync && st->now > s_last_sync) ? (uint32_t)(st->now - s_last_sync) : 0;
    struct tm tm; localtime_r(&st->now, &tm);
    strftime(st->local, sizeof st->local, "%Y-%m-%d %H:%M:%S %Z", &tm);
    strncpy(st->tz, s_tz, sizeof st->tz - 1);
}

const char *clock_source_name(clock_source_t s)
{
    static const char *n[] = { "none", "ntp", "app", "rtc" };
    return n[s & 3];
}

int64_t clock_utc_ms(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return time_valid(tv.tv_sec) ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}
bool    clock_synced_since_boot(void) { return s_synced_boot; }
int32_t clock_last_offset_ms(void)    { return s_last_offset_ms; }
bool    clock_last_offset_known(void) { return s_offset_known; }
void    clock_set_sync_hook(clock_sync_fn fn) { s_sync_hook = fn; }
int64_t clock_last_sync_utc_ms(void)  { return (int64_t)s_last_sync * 1000; }
void    clock_resync(void)            { if (s_sntp_started) { snapshot(); esp_netif_sntp_start(); } }
