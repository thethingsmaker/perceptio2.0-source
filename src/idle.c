#include "idle.h"
#include <math.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "sampler.h"
#include "imu.h"
#include "store.h"
#include "upload.h"
#include "ota.h"
#include "net.h"
#include "ble.h"
#include "power.h"
#include "app.h"

static const char *TAG = "IDLE";

static uint32_t s_still_s;          /* still seconds (one isolated moving second does not reset it) */
static uint32_t s_moving_s;         /* consecutive moving seconds */
static bool     s_have_prev;
static float    s_prev_mg[3];       /* gravity reference: the last still sample (or the one that restarted the count), mg */
static float    s_cur_mg[3];        /* this second's sample, adopted as the reference by the caller */
static bool     s_warned, s_fired;

/* why the timer is held at zero, or NULL when it may run */
static const char *busy_reason(void)
{
    if (store_is_recording())      return "recording";
    if (ble_live_active())         return "live";
    if (upload_active())           return "upload";
    if (ota_busy())                return "ota";
    net_test_t nt; net_test_get(&nt);
    if (nt.state == NT_RUNNING)    return "wifi test";
    bq25180_status_t bq; power_charger_status(&bq);
    if (bq.vin_pgood)              return "cable";
    int64_t last = ble_last_request_us();
    if (last && esp_timer_get_time() - last < (int64_t)IDLE_REQ_GRACE_S * 1000000) return "app";
    return NULL;
}

/* the current sample becomes the gravity reference: after a still second, or after a confirmed move (a knock's sample must
 * NOT become the reference, or the next resting sample would look like a second move) */
static void adopt_ref(void) { s_prev_mg[0] = s_cur_mg[0]; s_prev_mg[1] = s_cur_mg[1]; s_prev_mg[2] = s_cur_mg[2]; s_have_prev = true; }

/* one sample's verdict: true = the racket did not move in the last second; margins reported for the log */
static bool still_now(float *gyro_dps, float *dacc_mg)
{
    sampler_stats_t st; sampler_get_stats(&st);
    *gyro_dps = *dacc_mg = 0;
    if (st.rate_hz == 0) { s_have_prev = false; return false; }          /* IMU silent: no verdict */
    imu_scale_t sc = imu_get_scale();
    float gx = st.last.v[1] * sc.gy_mdps_lsb / 1000.0f, gy = st.last.v[2] * sc.gy_mdps_lsb / 1000.0f, gz = st.last.v[3] * sc.gy_mdps_lsb / 1000.0f;
    float ax = st.last.v[4] * sc.xl_mg_lsb, ay = st.last.v[5] * sc.xl_mg_lsb, az = st.last.v[6] * sc.xl_mg_lsb;   /* mg */
    *gyro_dps = sqrtf(gx * gx + gy * gy + gz * gz);
    if (s_have_prev) {
        float d = fabsf(ax - s_prev_mg[0]);
        if (fabsf(ay - s_prev_mg[1]) > d) d = fabsf(ay - s_prev_mg[1]);
        if (fabsf(az - s_prev_mg[2]) > d) d = fabsf(az - s_prev_mg[2]);
        *dacc_mg = d;
    }
    s_cur_mg[0] = ax; s_cur_mg[1] = ay; s_cur_mg[2] = az;
    return *gyro_dps < IDLE_GYRO_DPS && (!s_have_prev || *dacc_mg < IDLE_ACC_MG);
}

void idle_tick_1s(void)
{
    if (s_fired) return;
    uint32_t limit_s = (uint32_t)g_cfg.idle_off_min * 60u;
    const char *busy = limit_s ? busy_reason() : "disabled";
    if (busy) {
        if (s_still_s >= 60) ESP_LOGI(TAG, "timer reset after %" PRIu32 " min still: %s", s_still_s / 60, busy);
        s_still_s = 0; s_moving_s = 0; s_warned = false; s_have_prev = false;
        return;
    }
    float gyro, dacc;
    if (!still_now(&gyro, &dacc)) {
        if (++s_moving_s >= IDLE_MOVE_S) {                                /* a single knock / vibration spike does not count */
            if (s_still_s >= 60) ESP_LOGI(TAG, "moved after %" PRIu32 " min still (gyro %.1f dps, da %.0f mg)", s_still_s / 60, gyro, dacc);
            s_still_s = 0; s_warned = false; adopt_ref();
        }
        return;
    }
    adopt_ref();
    s_moving_s = 0;
    s_still_s++;
    if (s_still_s % 60 == 0) ESP_LOGI(TAG, "still %" PRIu32 " min (gyro %.1f dps, da %.0f mg)", s_still_s / 60, gyro, dacc);
    if (!s_warned && s_still_s + IDLE_WARN_S >= limit_s) {
        s_warned = true;
        ESP_LOGW(TAG, "still for %" PRIu32 " min: switching off in %d s unless moved (idle_off_min %" PRId32 ")", s_still_s / 60, IDLE_WARN_S, g_cfg.idle_off_min);
    }
    if (s_still_s >= limit_s) {
        s_fired = true;
        ESP_LOGW(TAG, "still for %" PRId32 " min: switching off", g_cfg.idle_off_min);
        app_power_off("idle");                                            /* main loop: cloud_shutdown, ship mode / CHARGE-IDLE */
    }
}

bool idle_warning(void) { return s_warned && !s_fired; }
