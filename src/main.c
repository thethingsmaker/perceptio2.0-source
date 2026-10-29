/*
 * PadelSense Perceptio 2.0 — firmware entry point.
 *
 * Boot order: config (NVS) → board buses → power/charger → LED → IMU → NAND → WiFi driver →
 * store (NAND inventory) → self-test (optional) → join WiFi → core-1 sampler (+ NAND writer) → mDNS + web/API on
 * core 0 → idle (LED, charge gate, button, session timeout).
 *
 * Core split (hard rule): core 0 = WiFi, lwIP, timers, HTTP/UI; core 1 = capture (sampler, later NAND writer),
 * GPIO ISRs. No internal-flash writes while a capture session runs.
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include <math.h>
#include "board.h"
#include "config.h"
#include "power.h"
#include "lp5810.h"
#include "led.h"
#include "clock.h"
#include "cloud.h"
#include "bus.h"
#include "app.h"
#include "imu.h"
#include "w25n01kv.h"
#include "net.h"
#include "sampler.h"
#include "store.h"
#include "upload.h"
#include "ota.h"
#include "rlog.h"
#include "selftest.h"
#include "webui.h"

static const char *TAG = "MAIN";

/* ------------------------------------------------------------------ */
static void status_cb(webui_status_t *o)
{
    sampler_stats_t st; sampler_get_stats(&st);
    imu_scale_t sc = imu_get_scale();
    const imu_cfg_t *ic = imu_get_cfg();
    bq25180_status_t bq; power_charger_status(&bq);

    o->uptime_s  = esp_timer_get_time() / 1000000;
    o->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    o->heap_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    o->vbat      = power_vbat_avg();                 /* averaged + smoothed gauge (power.h); a raw reading jumps ±50 mV */
    o->vbat_cal  = power_vbat_calibrated();
    o->soc_pct   = !power_battery_present() ? -1                /* -1 = no battery (USB only) -> null in the API */
                 : power_soc_display() >= 0 ? power_soc_display()
                 : (bq.vin_pgood && bq.chg == BQ_CHG_DONE) ? 100 : power_soc_pct(o->vbat, bq.chg == BQ_CHG_CC || bq.chg == BQ_CHG_CV);
    o->bq_stat0  = bq.stat0; o->bq_stat1 = bq.stat1;
    o->rssi      = net_rssi();
    strncpy(o->ip, net_ip(), sizeof o->ip - 1);
    o->odr_hz = ic->odr_hz; o->xl_fs_g = ic->xl_fs_g; o->gy_fs_dps = ic->gy_fs_dps; o->hg_fs_g = ic->hg_fs_g;
    o->xl_mg_lsb = sc.xl_mg_lsb; o->gy_mdps_lsb = sc.gy_mdps_lsb; o->hg_mg_lsb = sc.hg_mg_lsb;
    o->imu_rate_hz = st.rate_hz; o->imu_total = st.total; o->imu_missed = st.missed;
    o->imu_temp_c = 25.0f + st.last.v[0] / 256.0f;
    o->gx = st.last.v[1] * sc.gy_mdps_lsb / 1000; o->gy = st.last.v[2] * sc.gy_mdps_lsb / 1000; o->gz = st.last.v[3] * sc.gy_mdps_lsb / 1000;
    o->ax = st.last.v[4] * sc.xl_mg_lsb;          o->ay = st.last.v[5] * sc.xl_mg_lsb;          o->az = st.last.v[6] * sc.xl_mg_lsb;
    o->hx = st.last.v[7] * sc.hg_mg_lsb;          o->hy = st.last.v[8] * sc.hg_mg_lsb;          o->hz = st.last.v[9] * sc.hg_mg_lsb;
    o->hg_peak_g = st.hg_peak_g;
    o->tests_pass = selftest_passed(); o->tests_total = selftest_count();
    clock_get_status(&o->clk);
    cloud_get_status(&o->cloud);
    store_get_status(&o->store);
}

/* what the cloud link reports in hello/telemetry */
static void cloud_info_cb(cloud_info_t *o)
{
    static char cfg_json[512];
    sampler_stats_t st; sampler_get_stats(&st);
    imu_scale_t sc = imu_get_scale();
    const imu_cfg_t *ic = imu_get_cfg();
    bq25180_status_t bq; power_charger_status(&bq);
    store_status_t ss; store_get_status(&ss);
    o->state       = ss.state == REC_IDLE ? "idle" : ss.state == REC_RECORDING ? "recording" : "stopping";
    o->battery_pct = !power_battery_present() ? -1              /* -1 -> null to the backend (protocol: unknown = null) */
                   : power_soc_display() >= 0 ? power_soc_display()
                   : (bq.vin_pgood && bq.chg == BQ_CHG_DONE) ? 100 : power_soc_pct(power_vbat(), bq.chg == BQ_CHG_CC || bq.chg == BQ_CHG_CV);
    o->charging    = bq.chg == BQ_CHG_CC || bq.chg == BQ_CHG_CV;
    o->plugged     = bq.vin_pgood;
    o->rssi        = net_rssi();
    strncpy(o->ip, net_ip(), sizeof o->ip - 1);
    o->temp_c      = 25.0f + st.last.v[0] / 256.0f;
    o->storage_total_mb = ss.total_mb; o->storage_free_mb = ss.free_mb;
    o->heap_kb     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10;
    o->psram_kb    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10;
    o->rec_samples = ss.state != REC_IDLE ? ss.cur.samples : 0;
    o->dropped_samples = ss.state != REC_IDLE ? ss.cur.dropped : 0;
    o->odr_hz = ic->odr_hz; o->xl_fs_g = ic->xl_fs_g; o->hg_fs_g = ic->hg_fs_g;
    o->xl_mg_lsb = sc.xl_mg_lsb; o->gy_mdps_lsb = sc.gy_mdps_lsb; o->hg_mg_lsb = sc.hg_mg_lsb;
    config_dump_json(cfg_json, sizeof cfg_json);
    o->config_json = cfg_json;
}

/* live-apply of parameters edited from the UI / app (the reboot == false ones) */
static void on_cfg_change(const cfg_param_t *p)
{
    if      (!strcmp(p->key, "led_bright")) lp5810_set_brightness(g_cfg.led_bright);
    else if (!strcmp(p->key, "wifi_ps"))    net_set_power_save(g_cfg.wifi_ps);
    else if (!strcmp(p->key, "ws_hz"))      webui_set_stream_hz(g_cfg.ws_hz);
    else if (!strcmp(p->key, "ichg_ma"))    bq25180_set_ichg_ma(g_cfg.ichg_ma);
    else if (!strcmp(p->key, "ilim_ma"))    bq25180_set_ilim_ma(g_cfg.ilim_ma);
    else if (!strcmp(p->key, "tz"))         clock_set_tz(g_cfg.tz);
    else if (!strcmp(p->key, "log_serial")) rlog_set_serial(g_cfg.log_serial);
    else if (!strcmp(p->key, "log_remote")) rlog_set_level(g_cfg.log_remote);
}

/* No NTC on the board. The cell stays at ambient (measured: board 45 C while the cell is cold), so the board
 * temperature is used as an AMBIENT proxy with ~+23 K self-heating while charging: pause outside the Li-ion
 * charging window (0..45 C cell), resume with 5 K hysteresis. Thresholds are parameters. */
static bool charge_gate(float board_c)
{
    static bool paused;
    { bq25180_status_t bq; if (power_charger_status(&bq) == ESP_OK && bq.safety_fault && bq.vin_pgood && !paused) {
        ESP_LOGW(TAG, "charger safety-timer fault: restarting the charge cycle");
        bq25180_restart_charge(); } }
    bool too_hot = board_c > g_cfg.chg_tmax_c, too_cold = board_c < g_cfg.chg_tmin_c;
    if (!paused && (too_hot || too_cold)) {
        paused = true; bq25180_charge_enable(false);
        ESP_LOGW(TAG, "charge PAUSED: board %.1f C outside %ld..%ld C", board_c, (long)g_cfg.chg_tmin_c, (long)g_cfg.chg_tmax_c);
    } else if (paused && board_c < g_cfg.chg_tmax_c - 5 && board_c > g_cfg.chg_tmin_c + 5) {
        paused = false; bq25180_charge_enable(true);
        ESP_LOGW(TAG, "charge resumed: board %.1f C", board_c);
    }
    return paused;
}

#define DOUBLE_PRESS_US (1500 * 1000)            /* second press (its 0.3 s WAKE1) within 1.5 s of the first */
#define AP_PASSWORD "perceptio"                   /* fallback AP (no known WiFi). Same on every unit: development only */
static volatile power_btn_t s_btn_event;
static void on_button(power_btn_t k) { s_btn_event = k; }
static const char *volatile s_off_request;                 /* set by the UI/cloud; served by the main loop */
void app_request_power_off(const char *why) { s_off_request = why ? why : "ui"; }

/* ---- status LED: one place decides (priority order in led.h); called every second ---- */
static int64_t  s_boot_fail_until;               /* esp_timer time until which a failed self-test is shown */
static bool     s_chg_paused;                    /* charge_gate(): paused by the temperature window */
static uint32_t s_backend_bad_s;                 /* backend_watch(): seconds the backend has been unreachable */
static void led_update(const bq25180_status_t *bq)
{
    if (led_get() == LED_IDENTIFY) return;       /* "find me" / refused-start flash runs its own timer */
    static uint32_t notchg_s;
    bool bat = power_battery_present(), plugged = bq->vin_pgood;
    bool charging = bq->chg == BQ_CHG_CC || bq->chg == BQ_CHG_CV, done = bq->chg == BQ_CHG_DONE;
    notchg_s = (plugged && bat && !charging && !done) ? notchg_s + 1 : 0;     /* cable + cell but no charging */
    bool chg_fault = plugged && bat && (s_chg_paused || bq->safety_fault || notchg_s >= 10);
    bool net_bad = net_ap_active() || s_backend_bad_s >= 20;                  /* 20 s grace for a normal (re)connect */
    led_mode_t m;
    if (esp_timer_get_time() < s_boot_fail_until) m = LED_BOOT_FAIL;
    else if (store_is_recording())                m = LED_RECORDING;
    else if (chg_fault)                           m = LED_CHARGE_FAULT;
    else if (net_bad)                             m = LED_NET;
    else if (plugged && bat && done)              m = LED_FULL;
    else if (plugged && bat && charging)          m = LED_CHARGING;
    else                                          m = LED_OK;
    if (m != led_get()) led_set(m);
}

/* recording started / stopped (from whichever side did it: UI, cloud, button, timeout) */
static void on_store_event(bool started, const store_rec_t *r, const char *by)
{
    char d[160];
    if (started) {
        led_set(LED_RECORDING);                   /* immediate feedback; led_update() keeps it */
        bus_state("recording");
        snprintf(d, sizeof d, "{\"rec_id\":\"%s\",\"by\":\"%s\",\"rec_tag\":\"%s\"}", r->rec_id, by, r->rec_tag);
        bus_event("rec_started", d);
    } else {
        bus_state("idle");                        /* LED: led_update() picks the next state within 1 s */
        snprintf(d, sizeof d, "{\"rec_id\":\"%s\",\"by\":\"%s\",\"samples\":%" PRIu32 ",\"bytes\":%" PRIu32 ",\"duration_ms\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"complete\":%s}",
                 r->rec_id, by, r->samples, r->bytes, r->duration_ms, r->dropped, r->complete ? "true" : "false");
        bus_event("rec_stopped", d);
    }
    bus_inventory_changed();                        /* retained hello: current_rec / recordings[] are part of it */
}

/* Fallback AP "Perceptio-<device id>": the UI must always be reachable. Started when no saved network joins at boot,
 * and by backend_watch() whenever the backend has been unreachable for AP_AFTER_S (joined a network without internet,
 * DNS dead, router gone...). Switched off again once the backend has been reachable for 30 s and nobody uses the AP. */
#define AP_AFTER_S   90
static esp_err_t start_fallback_ap(const char *why)
{
    if (net_ap_active()) return ESP_OK;
    char ssid[33]; snprintf(ssid, sizeof ssid, "Perceptio-%s", cloud_device_id());
    ESP_LOGW(TAG, "fallback AP: %s", why);
    esp_err_t e = net_start_ap(ssid, AP_PASSWORD);
    if (e == ESP_OK) { net_set_power_save(false); webui_start(status_cb); }   /* webui_start is a no-op when running */
    return e;
}
static void backend_watch(void)                    /* 1 s tick */
{
    static uint32_t bad_s, good_s;
    cloud_status_t cs; cloud_get_status(&cs);
    /* "reachable" = the broker answered: online, or waiting for enrolment approval / rejected (still a live link) */
    bool ok = cs.state == CLOUD_ONLINE || cs.state == CLOUD_ENROL_WAIT || cs.state == CLOUD_ENROL_REJECTED
              || (cs.state == CLOUD_OFF && net_is_connected());          /* no cloud configured: WiFi is all we can check */
    if (ok) { bad_s = 0; good_s++; } else { good_s = 0; bad_s++; }
    s_backend_bad_s = bad_s;
    if (!net_ap_active() && bad_s == AP_AFTER_S) {
        char why[64]; snprintf(why, sizeof why, "backend unreachable for %d s (%s)", AP_AFTER_S, net_is_connected() ? "WiFi up, no internet?" : "no WiFi");
        start_fallback_ap(why);
    }
    if (net_ap_active() && good_s >= 30 && good_s % 30 == 0) net_stop_ap();    /* retried every 30 s while a phone is on it */
}

static void ota_mode_enter(void) { cloud_suspend("fw_update"); }

static void toggle_recording(const char *by)
{
    if (app_rec_toggle(by) != APP_OK) led_identify(1);     /* a short white flash: refused (reason in the APP: line) */
}

/* ------------------------------------------------------------------ */
/* Power states.  OFF = BQ ship mode (no SYS power).  CHARGE-IDLE = user switched off but a cable is in:
 * the BQ keeps SYS up, so we run a minimal loop (no WiFi/UI/sampler) showing charge status on the LED.
 * ON = everything running.  "user_off" in NVS carries the user's intent across power cycles.         */
/* ------------------------------------------------------------------ */
static void power_off_now(const char *why)
{
    ESP_LOGW(TAG, "switching OFF (%s)", why);
    if (store_is_recording()) store_stop("power_off", NULL);   /* trailer on the NAND before anything else */
    cloud_shutdown(why);                          /* tell the backend it is intentional (≤ 1.2 s), before anything else */
    ota_mark_valid();                             /* a deliberate power-off is not a reason to roll back a fresh image */
    led_set(LED_OFF);
    config_user_off_set(true);                    /* allowed: no capture session is running at this point */
    bq25180_status_t bq; power_charger_status(&bq);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (bq.vin_pgood) {
        esp_restart();                            /* cable in: ship mode cannot engage -> reboot into CHARGE-IDLE */
    } else {
        bq25180_enter_ship_mode();                /* on battery: BQ disconnects SYS within ~1 s */
        vTaskDelay(pdMS_TO_TICKS(6000));
        ESP_LOGE(TAG, "ship mode did not engage");
        power_deep_sleep_fallback(60);
    }
}

static void charge_idle_loop(void)
{
    ESP_LOGW(TAG, "CHARGE-IDLE: device is off, cable in. Hold the button 2 s to turn on.");
    s_btn_event = 0;
    uint32_t tick = 0;
    for (;;) {
        bq25180_status_t bq; power_charger_status(&bq);
        imu_sample_t smp; imu_read_sample(&smp);
        float board_c = 25.0f + smp.v[0] / 256.0f;
        bool paused = charge_gate(board_c);

        if (!bq.vin_pgood) {                      /* cable removed while off -> really off */
            ESP_LOGW(TAG, "cable removed -> ship mode");
            led_set(LED_OFF);
            bq25180_enter_ship_mode();
            vTaskDelay(pdMS_TO_TICKS(6000));
            power_deep_sleep_fallback(60);
        }
        { power_btn_t k = power_poll_button(); if (k) s_btn_event = k; }   /* poll as well as /INT */
        if (s_btn_event) {                        /* any press: turn on */
            ESP_LOGW(TAG, "button -> turning ON");
            config_user_off_set(false);
            led_set(LED_OFF);
            esp_restart();
        }

        led_set(paused ? LED_CHARGE_FAULT : bq.chg == BQ_CHG_DONE ? LED_FULL : LED_CHARGING);

        if (++tick % 50 == 0)                     /* every 2 s */
            ESP_LOGI(TAG, "CHARGE-IDLE  VBAT %.2f V  %-18s ILIM %d THERM %d  board %.1f C  STAT1 %02X /INT edges %" PRIu32 "%s",
                     power_vbat(), bq25180_chg_name(bq.chg), bq.ilim_active, bq.thermreg_active, board_c, bq.stat1, power_int_edges(),
                     paused ? "  (charge paused)" : "");
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ------------------------------------------------------------------ */
void app_main(void)
{
    rlog_init();                                  /* capture every log line from here on (shipped once the cloud is up) */
    vTaskDelay(pdMS_TO_TICKS(1500));              /* let the USB CDC console enumerate */
    ESP_LOGI(TAG, "==== PadelSense Perceptio 2.0 — fw %s, hw rev %d ====", FW_VERSION_STR, PERCEPTIO_HW_REV);

    ESP_ERROR_CHECK(config_init());
    rlog_set_serial(g_cfg.log_serial);
    rlog_set_level(g_cfg.log_remote);
    clock_init(g_cfg.tz);
    ESP_ERROR_CHECK(board_init());
    power_init();
    bq25180_set_ichg_ma(g_cfg.ichg_ma);
    bq25180_set_ilim_ma(g_cfg.ilim_ma);
    power_set_button_hook(on_button);
    lp5810_init();
    lp5810_set_brightness(g_cfg.led_bright);
    led_start();
    led_set(LED_BOOT);

    bool imu_ok = imu_init() == ESP_OK;
    if (imu_ok) {
        imu_cfg_t ic = { .odr_hz = g_cfg.odr_hz, .xl_fs_g = g_cfg.xl_fs_g, .gy_fs_dps = g_cfg.gy_fs_dps, .hg_fs_g = g_cfg.hg_fs_g };
        imu_ok = imu_configure(&ic) == ESP_OK;
    }

    /* user had switched the device off and a cable is in -> charge quietly, don't "turn on" */
    {
        bq25180_status_t bq; power_charger_status(&bq);
        if (config_user_off_get()) {
            if (bq.vin_pgood) charge_idle_loop();     /* never returns */
            config_user_off_set(false);               /* on battery with the flag set: ship mode didn't happen; run */
        }
    }
    bool nand_ok = nand_init(40 * 1000 * 1000) == ESP_OK;
    ESP_LOGI(TAG, "heap after drivers: %u KB internal", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    net_init();
    ESP_LOGI(TAG, "heap after wifi init: %u KB internal", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    cloud_init(cloud_info_cb);                    /* identity, boot_id++, buffered boot event; connects when the link is up */
    net_add_link_hook(clock_link);                /* NTP on every got-IP (first, as before) */
    net_add_link_hook(cloud_link);                /* MQTT client start/stop */
    { cloud_status_t cs; cloud_get_status(&cs); store_init(cloud_device_id(), cs.boot_id, nand_ok); }
    store_set_event_hook(on_store_event);
    upload_init(cloud_upload_done, cloud_device_id());               /* resumable HTTPS upload task (core 0), driven by the `upload` command */
    ota_init();                                   /* GitHub Releases updater; this image may still be PENDING_VERIFY */
    ota_set_update_mode(ota_mode_enter, cloud_resume);   /* an install runs with the cloud link down (frees its RAM) */

    if (g_cfg.selftest_boot) {
        selftest_run(ST_QUICK | (g_cfg.selftest_full ? ST_NAND_STRESS : 0), g_cfg.selftest_full ? 30 : 0);
    } else {
        selftest_run(ST_SOC, 0);                  /* always know what we booted on */
    }

    bool online = net_connect_from_list(12000) == ESP_OK;
    ESP_LOGI(TAG, "heap after wifi join: %u KB internal", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));

    config_set_change_hook(on_cfg_change);
    webui_set_stream_hz(g_cfg.ws_hz);
    if (imu_ok) { sampler_add_sink(store_feed); sampler_add_sink(webui_feed); sampler_start(); }

    if (online) {
        net_set_power_save(g_cfg.wifi_ps);
        net_start_mdns(g_cfg.dev_name, config_fw_version());
        webui_start(status_cb);
        ESP_LOGI(TAG, ">>> http://%s/ <<<  heap %u KB internal (MQTT not yet connected)", net_ip(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    } else {
        /* No known network: own access point "Perceptio-<device id>", same UI at http://192.168.4.1/. The saved
         * networks keep being retried in the background (net.c); joining one switches the AP off. */
        if (start_fallback_ap("no saved network reachable at boot") == ESP_OK) {
            net_set_power_save(false);
            net_start_mdns(g_cfg.dev_name, config_fw_version());
            webui_start(status_cb);
            ESP_LOGW(TAG, ">>> no WiFi: join 'Perceptio-%s' (password '%s') and open http://%s/ <<<", cloud_device_id(), AP_PASSWORD, net_ip());
        } else {
            ESP_LOGE(TAG, "no WiFi and the fallback AP failed — UI not reachable");
        }
    }

    bool healthy = imu_ok && nand_ok && selftest_passed() == selftest_count();
    if (!healthy) { s_boot_fail_until = esp_timer_get_time() + 3000000; led_set(LED_BOOT_FAIL); }   /* red blink 3 s */
    else led_set(LED_OK);
    ESP_LOGI(TAG, "boot complete: imu %s, nand %s, wifi %s, selftest %d/%d", imu_ok ? "ok" : "FAIL", nand_ok ? "ok" : "FAIL",
             online ? net_ssid() : "offline", selftest_passed(), selftest_count());

    /* idle (100 ms tick): green heartbeat = healthy, red fast blink = fault; charger line every 2 s; button handling */
    uint32_t tick = 0;
    s_btn_event = 0;
    for (;;) {
        { power_btn_t k = power_poll_button(); if (k) s_btn_event = k; }
        /* Button = two gestures only (the BQ25180 reports a press after 0.3 s of hold = WAKE1, and 3 s = WAKE2; there is
         * no release event):  hold 3 s -> power off;  two presses within 1.5 s -> start/stop recording. A single press
         * does nothing — a long press always begins with a WAKE1, which used to start a recording on the way to off. */
        {
            static int64_t first_us;                              /* WAKE1 of a possible double press (0 = none) */
            int64_t now = esp_timer_get_time();
            if (s_btn_event == BTN_LONG) { first_us = 0; power_off_now("button"); }   /* closes a running session first */
            else if (s_btn_event == BTN_SHORT) {
                if (first_us && now - first_us <= DOUBLE_PRESS_US) {
                    first_us = 0;
                    ESP_LOGI(TAG, "button: double press");
                    toggle_recording("button");
                } else first_us = now;
            }
            if (first_us && now - first_us > DOUBLE_PRESS_US) { first_us = 0; ESP_LOGI(TAG, "button: single press (ignored)"); }
        }
        if (s_off_request) power_off_now(s_off_request);            /* UI "Power off" */

        /* First boot after an OTA: confirm the image once the cloud is up (or after 90 s without it), never mid-session.
         * Until then a crash/reset rolls back to the previous slot (bootloader rollback). */
        if (ota_pending_verify() && !store_is_recording()) {
            cloud_status_t cs; cloud_get_status(&cs);
            if (cs.state == CLOUD_ONLINE || tick > 900) {
                ota_mark_valid();
                char d[80]; snprintf(d, sizeof d, "{\"from\":\"%s\",\"to\":\"%s\"}", config_prev_fw_version(), config_fw_version());
                bus_event("fw_updated", d);
            }
        }
        s_btn_event = 0;

        /* session guards: max length (parameter) and storage exhausted by the writer */
        if (store_is_recording()) {
            store_status_t ss; store_get_status(&ss);
            if (ss.storage_full) app_rec_stop("storage_full", NULL, NULL);
            else if (ss.elapsed_ms >= (uint32_t)g_cfg.session_max_min * 60000u) app_rec_stop("timeout", NULL, NULL);
        }

        if (tick % 10 == 0) backend_watch();          /* fallback AP when the backend stays unreachable */
        if (tick % 10 == 0) {                         /* battery gauge: 1 s ticks */
            bq25180_status_t b1; power_charger_status(&b1);
            power_battery_tick(b1.vin_pgood, b1.chg == BQ_CHG_CC || b1.chg == BQ_CHG_CV, b1.chg == BQ_CHG_DONE);
            led_update(&b1);
        }
        if (tick % 100 == 0) {                        /* status line every 10 s (it is also shipped to the backend log) */
            bq25180_status_t bq; power_charger_status(&bq);
            sampler_stats_t st; sampler_get_stats(&st);
            float board_c = 25.0f + st.last.v[0] / 256.0f;
            s_chg_paused = charge_gate(board_c);
            ESP_LOGI(TAG, "VBAT %.2f V (avg %.3f, %d %%)  VIN %d  %-18s ILIM %d VINDPM %d VDPPM %d THERM %d%s  board %.1f C | IMU %" PRIu32 " Hz, peak %.1f g | heap %u KB",
                     power_vbat(), power_vbat_avg(), power_soc_display(), bq.vin_pgood, bq25180_chg_name(bq.chg), bq.ilim_active, bq.vindpm_active, bq.vdppm_active, bq.thermreg_active,
                     bq.safety_fault ? " SAFETY-TMR" : "", board_c,
                     st.rate_hz, st.hg_peak_g, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
        }
        if (tick % 600 == 0) {                        /* charger registers once a minute: the truth behind the status words */
            uint8_t r[13]; char hex[64]; int n = 0;
            for (int i = 0; i < 13; i++) {
                r[i] = 0;
                if (bq25180_read(i, &r[i]) == ESP_OK) n += snprintf(hex + n, sizeof hex - n, "%02X ", r[i]);
                else n += snprintf(hex + n, sizeof hex - n, "?? ");
            }
            ESP_LOGI(TAG, "BQ regs 00-0C: %s(STAT0 STAT1 FLAG0 VBAT ICHG CTRL0 CTRL1 IC TMR SHIP SYS TS MASK)", hex);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
