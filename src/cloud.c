#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "cloud.h"
#include "bus.h"
#include "app.h"
#include "cloud_secrets.h"
#include "config.h"
#include "board.h"
#include "clock.h"
#include "led.h"
#include "store.h"
#include "upload.h"
#include "rlog.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "mqtt5_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "CLOUD";

#define BROKER_URI      "wss://mqtt.padelsense.net:443/mqtt"
#define NS              "p2"
#define EV_RING         256
#define CMD_HISTORY     64
#define TELEMETRY_MS    10000
#define ENROL_RESEND_MS 60000
#define REJECT_RETRY_MS 600000
#define SNTP_PERIOD_MS  600000

/* ------------------------------------------------------------------ */
typedef struct { uint32_t boot_id, seq; int64_t utc_ms; uint32_t mono_ms; char type[16]; char data[192]; int msg_id; bool sent; } ev_t;
typedef struct { char id[24]; char status[10]; char reason[16]; } cmd_hist_t;

static cloud_info_fn   s_info_fn;
static char            s_devid[13];
static uint32_t        s_boot_id;
static char            s_user[16], s_pass[72];
static bool            s_enrolled, s_link_up, s_connected;
static cloud_state_t   s_state = CLOUD_OFF;
static esp_mqtt_client_handle_t s_cli;
static char            s_nonce[33];
static int             s_refusals;
static char            s_dev_state[16] = "idle";
static volatile bool   s_state_dirty, s_hello_dirty;
static int64_t         s_state_since_ms;
static uint32_t        s_seq;
static ev_t           *s_ev;                       /* PSRAM ring */
static uint32_t        s_ev_head, s_ev_count;       /* head = next write; count <= EV_RING */
static cmd_hist_t      s_hist[CMD_HISTORY]; static int s_hist_n;
static cloud_status_t  s_status;
static TaskHandle_t    s_task;
static int64_t         s_reconnect_due;
#define RECONNECT_MS 5000
static SemaphoreHandle_t s_puback;
static volatile int    s_wait_msg_id;
static int64_t         s_last_enrol_tx, s_last_telemetry, s_last_sntp, s_rejected_at;
static char            s_topic[64];

/* ------------------------------------------------------------------ */
/* identity + NVS                                                      */
/* ------------------------------------------------------------------ */
static void load_identity(void)
{
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    snprintf(s_devid, sizeof s_devid, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t b = 0; nvs_get_u32(h, "boot_id", &b);
    s_boot_id = b + 1;
    nvs_set_u32(h, "boot_id", s_boot_id); nvs_commit(h);
    size_t n = sizeof s_user; if (nvs_get_str(h, "user", s_user, &n) != ESP_OK) s_user[0] = 0;
    n = sizeof s_pass;        if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
    s_enrolled = s_pass[0] != 0;
    /* TODO: NVS encryption (with flash encryption, block 6) — until then the credential is readable from flash */
}

static void save_credential(const char *user, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (user && pass) { nvs_set_str(h, "user", user); nvs_set_str(h, "pass", pass); }
    else              { nvs_erase_key(h, "user"); nvs_erase_key(h, "pass"); }
    nvs_commit(h); nvs_close(h);
    strncpy(s_user, user ? user : "", sizeof s_user - 1);
    strncpy(s_pass, pass ? pass : "", sizeof s_pass - 1);
    s_enrolled = s_pass[0] != 0;
}

static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";          /* reflash over USB-Serial-JTAG */
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH:return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP:return "cpu_lockup";
    default:                return "unknown";
    }
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* ------------------------------------------------------------------ */
/* publish helpers (all on the MQTT task / cloud task, core 0)         */
/* ------------------------------------------------------------------ */
static const char *topic(const char *leaf)
{
    snprintf(s_topic, sizeof s_topic, "perceptio/2/%s/%s", s_devid, leaf);
    return s_topic;
}

static int pub_json(const char *t, cJSON *j, int qos, bool retain)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return -1;
    int id = -1;
    if (s_cli && s_connected) id = esp_mqtt_client_publish(s_cli, t, s, 0, qos, retain);
    free(s);
    return id;
}

static void publish_state(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "online", true);
    cJSON_AddStringToObject(j, "state", s_dev_state);
    { store_status_t ss; store_get_status(&ss); if (ss.state != REC_IDLE) cJSON_AddStringToObject(j, "rec_id", ss.cur.rec_id); }
    if (s_state_since_ms) cJSON_AddNumberToObject(j, "since_utc_ms", (double)s_state_since_ms); else cJSON_AddNullToObject(j, "since_utc_ms");
    cJSON_AddNumberToObject(j, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(j, "seq", s_seq);
    pub_json(topic("state"), j, 1, true);
}

static void publish_hello(void)
{
    cloud_info_t in = { 0 }; if (s_info_fn) s_info_fn(&in);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "proto", 2);
    cJSON_AddStringToObject(j, "device_id", s_devid);
    cJSON_AddStringToObject(j, "model", "perceptio-2");
    char hw[8]; snprintf(hw, sizeof hw, "%d", PERCEPTIO_HW_REV);
    cJSON_AddStringToObject(j, "hw_rev", hw);
    cJSON_AddStringToObject(j, "fw", config_fw_version());
    cJSON_AddStringToObject(j, "build", __DATE__ " " __TIME__);
    cJSON_AddNumberToObject(j, "boot_id", s_boot_id);
    cJSON_AddStringToObject(j, "reset_reason", reset_reason_name());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON *c = cJSON_AddObjectToObject(j, "clock");
    cJSON_AddNumberToObject(c, "utc_ms", (double)clock_utc_ms());
    cJSON_AddBoolToObject  (c, "synced", clock_synced_since_boot());
    if (clock_last_offset_known()) cJSON_AddNumberToObject(c, "offset_ms", clock_last_offset_ms()); else cJSON_AddNullToObject(c, "offset_ms");
    if (clock_last_sync_utc_ms()) cJSON_AddNumberToObject(c, "last_sync_utc_ms", (double)clock_last_sync_utc_ms()); else cJSON_AddNullToObject(c, "last_sync_utc_ms");
    cJSON_AddStringToObject(j, "state", s_dev_state);
    store_status_t ss; store_get_status(&ss);
    if (ss.state != REC_IDLE) {
        cJSON *cr = cJSON_AddObjectToObject(j, "current_rec");
        cJSON_AddStringToObject(cr, "rec_id", ss.cur.rec_id);
        cJSON_AddStringToObject(cr, "rec_tag", ss.cur.rec_tag);
        if (ss.cur.start_utc_ms) cJSON_AddNumberToObject(cr, "started_utc_ms", (double)ss.cur.start_utc_ms); else cJSON_AddNullToObject(cr, "started_utc_ms");
        cJSON_AddNumberToObject(cr, "samples", ss.cur.samples);
        cJSON_AddNumberToObject(cr, "rate_hz", ss.cur.odr_hz);
    }
    cJSON *recs = cJSON_AddArrayToObject(j, "recordings");     /* inventory: everything stored (upload protocol = next spec) */
    int listed = 0;
    for (int i = 0; i < store_count() && listed < 16; i++) {    /* ≤ 16 entries keeps hello well under the 16 KB packet limit */
        const store_rec_t *r = store_rec(i);
        if (!r) continue;
        listed++;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "rec_id", r->rec_id);
        cJSON_AddStringToObject(o, "rec_tag", r->rec_tag);
        cJSON_AddNumberToObject(o, "bytes", (double)r->bytes);
        cJSON_AddNumberToObject(o, "samples", r->samples);
        cJSON_AddNumberToObject(o, "uploaded_bytes", r->uploaded_bytes);
        cJSON_AddBoolToObject  (o, "complete", r->complete);
        if (r->start_utc_ms) cJSON_AddNumberToObject(o, "started_utc_ms", (double)r->start_utc_ms); else cJSON_AddNullToObject(o, "started_utc_ms");
        cJSON_AddNumberToObject(o, "duration_ms", r->duration_ms);
        cJSON_AddItemToArray(recs, o);
    }
    cJSON *st = cJSON_AddObjectToObject(j, "storage");
    cJSON_AddNumberToObject(st, "total_mb", in.storage_total_mb);
    cJSON_AddNumberToObject(st, "free_mb", in.storage_free_mb);
    cJSON *pw = cJSON_AddObjectToObject(j, "power");
    if (in.battery_pct >= 0) cJSON_AddNumberToObject(pw, "battery_pct", in.battery_pct); else cJSON_AddNullToObject(pw, "battery_pct");
    cJSON_AddBoolToObject  (pw, "charging", in.charging);
    cJSON_AddBoolToObject  (pw, "plugged", in.plugged);
    cJSON *nt = cJSON_AddObjectToObject(j, "net");
    cJSON_AddStringToObject(nt, "ip", in.ip);
    cJSON_AddNumberToObject(nt, "rssi", in.rssi);
    cJSON *caps = cJSON_AddObjectToObject(j, "caps");
    cJSON_AddNumberToObject(caps, "gyro", 1);
    cJSON *acc = cJSON_AddArrayToObject(caps, "accel");
    cJSON *a0 = cJSON_CreateObject(); cJSON_AddNumberToObject(a0, "fsr_g", in.xl_fs_g); cJSON_AddNumberToObject(a0, "mg_per_lsb", in.xl_mg_lsb); cJSON_AddItemToArray(acc, a0);
    cJSON *a1 = cJSON_CreateObject(); cJSON_AddNumberToObject(a1, "fsr_g", in.hg_fs_g); cJSON_AddNumberToObject(a1, "mg_per_lsb", in.hg_mg_lsb); cJSON_AddItemToArray(acc, a1);
    cJSON *rh = cJSON_AddArrayToObject(caps, "rate_hz");
    cJSON_AddItemToArray(rh, cJSON_CreateNumber(in.odr_hz));            /* 960 on this hardware, not 1000 */
    cJSON_AddNumberToObject(caps, "gyro_mdps_per_lsb", in.gy_mdps_lsb);
    char hash[8]; snprintf(hash, sizeof hash, "%06" PRIx32, fnv1a(in.config_json) & 0xFFFFFF);
    cJSON_AddStringToObject(j, "config_hash", hash);
    pub_json(topic("hello"), j, 1, true);
    s_status.last_hello_utc_ms = clock_utc_ms();
}

static void publish_telemetry(void)
{
    cloud_info_t in = { 0 }; if (s_info_fn) s_info_fn(&in);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "utc_ms", (double)clock_utc_ms());
    if (in.battery_pct >= 0) cJSON_AddNumberToObject(j, "battery_pct", in.battery_pct); else cJSON_AddNullToObject(j, "battery_pct");
    cJSON_AddBoolToObject  (j, "charging", in.charging);
    cJSON_AddNumberToObject(j, "rssi", in.rssi);
    cJSON_AddNumberToObject(j, "temp_c", in.temp_c);
    cJSON_AddNumberToObject(j, "free_mb", in.storage_free_mb);
    cJSON_AddNumberToObject(j, "heap_kb", in.heap_kb);
    cJSON_AddNumberToObject(j, "psram_kb", in.psram_kb);
    cJSON_AddNumberToObject(j, "rec_samples", in.rec_samples);
    cJSON_AddNumberToObject(j, "dropped_samples", in.dropped_samples);
    pub_json(topic("telemetry"), j, 0, false);
}

/* Terminal acks must never be lost (the backend closes the command on them, and upload stats travel in them):
 * an ack that cannot be published right now is queued and flushed by the cloud task after the next hello. */
#define ACK_Q 8
typedef struct { bool used; char json[1024]; } ack_q_t;
static ack_q_t *s_ackq;                                  /* PSRAM */

static void ack_enqueue(const char *json)
{
    if (!s_ackq) return;
    for (int i = 0; i < ACK_Q; i++) if (!s_ackq[i].used) { strncpy(s_ackq[i].json, json, sizeof s_ackq[i].json - 1); s_ackq[i].used = true; return; }
    ESP_LOGW(TAG, "ack queue full: oldest dropped");
    memmove(&s_ackq[0], &s_ackq[1], (ACK_Q - 1) * sizeof *s_ackq);
    strncpy(s_ackq[ACK_Q - 1].json, json, sizeof s_ackq[0].json - 1); s_ackq[ACK_Q - 1].used = true;
}

static void ack_flush(void)                              /* cloud task, connected */
{
    if (!s_ackq || !s_connected || !s_enrolled) return;
    for (int i = 0; i < ACK_Q; i++) {
        if (!s_ackq[i].used) continue;
        int id = esp_mqtt_client_publish(s_cli, topic("ack"), s_ackq[i].json, 0, 1, 0);
        if (id < 0) return;                              /* still not possible: keep it */
        s_ackq[i].used = false;
    }
}

/* defer = true: never publish from the caller (foreign task / possibly offline) — queue for the cloud task */
static void ack_ex(const char *id, const char *status, const char *reason, cJSON *result, bool defer)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddStringToObject(j, "status", status);
    cJSON_AddNumberToObject(j, "utc_ms", (double)clock_utc_ms());
    if (reason) cJSON_AddStringToObject(j, "reason", reason);
    if (result) cJSON_AddItemToObject(j, "result", result);
    if (defer || !s_connected) {
        char *txt = cJSON_PrintUnformatted(j); cJSON_Delete(j);
        if (txt) { ack_enqueue(txt); free(txt); }
        if (s_task) xTaskNotify(s_task, 2, eSetBits);
        return;
    }
    if (pub_json(topic("ack"), j, 1, false) < 0) ESP_LOGW(TAG, "ack %s not published", id);   /* pub_json freed j */
}
static void ack(const char *id, const char *status, const char *reason, cJSON *result) { ack_ex(id, status, reason, result, false); }

/* ------------------------------------------------------------------ */
/* events: ring in PSRAM, replayed in order after every hello          */
/* ------------------------------------------------------------------ */
static void ev_publish(ev_t *e)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "boot_id", e->boot_id);
    cJSON_AddNumberToObject(j, "seq", e->seq);
    if (e->utc_ms) cJSON_AddNumberToObject(j, "utc_ms", (double)e->utc_ms); else cJSON_AddNullToObject(j, "utc_ms");
    cJSON_AddNumberToObject(j, "mono_ms", e->mono_ms);
    cJSON_AddStringToObject(j, "type", e->type);
    cJSON *d = e->data[0] ? cJSON_Parse(e->data) : NULL;
    if (d) cJSON_AddItemToObject(j, "data", d); else cJSON_AddNullToObject(j, "data");
    e->msg_id = pub_json(topic("event"), j, 1, false);
}

void cloud_event(const char *type, const char *data_json)
{
    if (!s_ev) return;
    ev_t *e = &s_ev[s_ev_head];
    memset(e, 0, sizeof *e);
    e->boot_id = s_boot_id; e->seq = s_seq++;
    e->utc_ms = clock_utc_ms(); e->mono_ms = esp_timer_get_time() / 1000;
    strncpy(e->type, type, sizeof e->type - 1);
    if (data_json) strncpy(e->data, data_json, sizeof e->data - 1);
    s_ev_head = (s_ev_head + 1) % EV_RING;
    if (s_ev_count < EV_RING) s_ev_count++;
    /* Never publish from the caller's task (main, esp_timer, httpd, sampler hooks...): a TLS write needs
     * stack those tasks do not have. The cloud task flushes it within milliseconds. */
    if (s_task) xTaskNotify(s_task, 2, eSetBits);
    /* TODO (block 2): mirror to NAND so a reboot does not lose buffered events */
}

/* Publish queued events in order, at most EV_WINDOW unacknowledged at a time: the broker allows 20 in-flight
 * per client (spec §13) and hello/state/telemetry share that budget — exceeding it is a protocol error and the
 * broker drops the connection before any PUBACK arrives, which turns into an endless reconnect loop. */
#define EV_WINDOW 8
static void ev_flush(void)
{
    if (!s_connected || !s_enrolled) return;
    uint32_t start = (s_ev_head + EV_RING - s_ev_count) % EV_RING;
    int inflight = 0;
    for (uint32_t i = 0; i < s_ev_count; i++) { ev_t *e = &s_ev[(start + i) % EV_RING]; if (!e->sent && e->msg_id > 0) inflight++; }
    for (uint32_t i = 0; i < s_ev_count && inflight < EV_WINDOW; i++) {
        ev_t *e = &s_ev[(start + i) % EV_RING];
        if (!e->sent && e->msg_id <= 0) { ev_publish(e); if (e->msg_id > 0) inflight++; }
    }
}

static void ev_replay(void)                         /* on (re)connect: in-flight ids from the old session are void */
{
    uint32_t start = (s_ev_head + EV_RING - s_ev_count) % EV_RING;
    for (uint32_t i = 0; i < s_ev_count; i++) { ev_t *e = &s_ev[(start + i) % EV_RING]; if (!e->sent) e->msg_id = 0; }
    ev_flush();
}

static void ev_mark_sent(int msg_id)
{
    for (uint32_t i = 0; i < EV_RING; i++) if (s_ev[i].msg_id == msg_id && msg_id > 0) { s_ev[i].sent = true; s_ev[i].msg_id = 0; }
}

static uint32_t ev_pending(void)
{
    uint32_t n = 0;
    uint32_t start = (s_ev_head + EV_RING - s_ev_count) % EV_RING;
    for (uint32_t i = 0; i < s_ev_count; i++) if (!s_ev[(start + i) % EV_RING].sent) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
static cmd_hist_t *hist_find(const char *id)
{
    for (int i = 0; i < CMD_HISTORY; i++) if (s_hist[i].id[0] && strcmp(s_hist[i].id, id) == 0) return &s_hist[i];
    return NULL;
}
static void hist_put(const char *id, const char *status, const char *reason)
{
    cmd_hist_t *h = &s_hist[s_hist_n++ % CMD_HISTORY];
    memset(h, 0, sizeof *h);
    strncpy(h->id, id, sizeof h->id - 1); strncpy(h->status, status, sizeof h->status - 1);
    if (reason) strncpy(h->reason, reason, sizeof h->reason - 1);
}

static void handle_cmd(const char *payload, int len)
{
    cJSON *j = cJSON_ParseWithLength(payload, len);
    const cJSON *id = j ? cJSON_GetObjectItem(j, "id") : NULL;
    const cJSON *cmd = j ? cJSON_GetObjectItem(j, "cmd") : NULL;
    const cJSON *exp = j ? cJSON_GetObjectItem(j, "expires_utc_ms") : NULL;
    const cJSON *args = j ? cJSON_GetObjectItem(j, "args") : NULL;
    if (!cJSON_IsString(id) || !cJSON_IsString(cmd)) { ESP_LOGW(TAG, "cmd: malformed"); cJSON_Delete(j); return; }
    s_status.cmds_received++;
    strncpy(s_status.last_cmd, cmd->valuestring, sizeof s_status.last_cmd - 1);

    cmd_hist_t *h = hist_find(id->valuestring);
    if (h) { ack(id->valuestring, h->status, h->reason[0] ? h->reason : NULL, NULL); cJSON_Delete(j); return; }   /* duplicate */

    int64_t now = clock_utc_ms();
    if (cJSON_IsNumber(exp) && now && now > (int64_t)exp->valuedouble) {
        hist_put(id->valuestring, "rejected", "expired");
        ack(id->valuestring, "rejected", "expired", NULL);
        ESP_LOGW(TAG, "cmd %s '%s' expired", id->valuestring, cmd->valuestring);
        cJSON_Delete(j); return;
    }
    ack(id->valuestring, "received", NULL, NULL);
    ESP_LOGI(TAG, "cmd %s '%s'", id->valuestring, cmd->valuestring);

    if (strcmp(cmd->valuestring, "identify") == 0) {
        const cJSON *sec = args ? cJSON_GetObjectItem(args, "seconds") : NULL;
        app_identify(cJSON_IsNumber(sec) ? (uint32_t)sec->valuedouble : 5, "cloud");
        hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, NULL);
    } else if (strcmp(cmd->valuestring, "sync_time") == 0) {
        clock_resync();
        vTaskDelay(pdMS_TO_TICKS(3000));                     /* SNTP round trip; we are on the MQTT task, this is fine */
        cJSON *r = cJSON_CreateObject();
        if (clock_last_offset_known()) cJSON_AddNumberToObject(r, "offset_ms", clock_last_offset_ms()); else cJSON_AddNullToObject(r, "offset_ms");
        hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, r);
        /* the time_synced event itself comes from the clock hook, like every other sync */
    } else if (strcmp(cmd->valuestring, "start_recording") == 0) {
        const cJSON *tag = args ? cJSON_GetObjectItem(args, "rec_tag") : NULL;
        const cJSON *rate = args ? cJSON_GetObjectItem(args, "rate_hz") : NULL;
        cloud_info_t in = { 0 }; if (s_info_fn) s_info_fn(&in);
        char rid[32] = "";
        const char *why = NULL;
        if (tag && !cJSON_IsString(tag)) why = "bad_args";
        else if (tag && strlen(tag->valuestring) > 64) why = "bad_args";
        else if (cJSON_IsNumber(rate) && (int)rate->valuedouble != in.odr_hz) why = "bad_args";   /* ODR is a reboot parameter */
        else {                                               /* tag length, battery, NAND: decided by the command layer */
            app_err_t e = app_rec_start(cJSON_IsString(tag) ? tag->valuestring : NULL, "cloud", rid);
            if (e != APP_OK) why = app_reason(e);
        }
        if (why) { hist_put(id->valuestring, "rejected", why); ack(id->valuestring, "rejected", why, NULL); }
        else {
            cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "rec_id", rid);
            hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, r);
        }
    } else if (strcmp(cmd->valuestring, "stop_recording") == 0) {
        const cJSON *rid = args ? cJSON_GetObjectItem(args, "rec_id") : NULL;
        const char *why = NULL;
        store_rec_t done;
        app_err_t e = app_rec_stop("cloud", cJSON_IsString(rid) ? rid->valuestring : NULL, &done);
        if (e != APP_OK) why = app_reason(e);
        if (why) { hist_put(id->valuestring, "rejected", why); ack(id->valuestring, "rejected", why, NULL); }
        else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "rec_id", done.rec_id);
            cJSON_AddNumberToObject(r, "samples", done.samples);
            cJSON_AddNumberToObject(r, "bytes", (double)done.bytes);
            cJSON_AddNumberToObject(r, "duration_ms", done.duration_ms);
            hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, r);
        }
    } else if (strcmp(cmd->valuestring, "upload") == 0) {
        const cJSON *rid = args ? cJSON_GetObjectItem(args, "rec_id") : NULL;
        const cJSON *url = args ? cJSON_GetObjectItem(args, "url") : NULL;
        const cJSON *tok = args ? cJSON_GetObjectItem(args, "token") : NULL;
        const cJSON *chk = args ? cJSON_GetObjectItem(args, "chunk_bytes") : NULL;
        const char *why = NULL;
        if (!cJSON_IsString(rid) || !cJSON_IsString(url) || !cJSON_IsString(tok)) why = "bad_args";
        else {
            esp_err_t e = upload_start(id->valuestring, rid->valuestring, url->valuestring, tok->valuestring, cJSON_IsNumber(chk) ? (uint32_t)chk->valuedouble : 0);
            if (e == ESP_ERR_INVALID_STATE) why = "busy"; else if (e == ESP_ERR_NOT_FOUND) why = "unknown_recording"; else if (e != ESP_OK) why = "bad_args";
        }
        if (why) { hist_put(id->valuestring, "rejected", why); ack(id->valuestring, "rejected", why, NULL); }
        /* else: `received` was sent above; the terminal ack comes from cloud_upload_done() */
    } else if (strcmp(cmd->valuestring, "delete_recording") == 0) {
        const cJSON *rid = args ? cJSON_GetObjectItem(args, "rec_id") : NULL;
        app_err_t e = cJSON_IsString(rid) ? app_rec_delete(rid->valuestring, "cloud") : APP_BAD_ARGS;
        /* protocol v2 answers an unknown rec_id with "bad_args" (as before the command layer) */
        const char *why = e == APP_OK ? NULL : e == APP_BUSY ? "busy" : "bad_args";
        if (why) { hist_put(id->valuestring, "rejected", why); ack(id->valuestring, "rejected", why, NULL); }
        else { hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, NULL); cloud_inventory_changed(); }
    } else if (strcmp(cmd->valuestring, "reboot") == 0) {
        hist_put(id->valuestring, "applied", NULL); ack(id->valuestring, "applied", NULL, NULL);
        app_reboot("cloud");                                 /* waits 1 s for the PUBACK; now also confirms a fresh OTA image */
    } else {
        hist_put(id->valuestring, "rejected", "unsupported"); ack(id->valuestring, "rejected", "unsupported", NULL);
    }
    cJSON_Delete(j);
}

/* ------------------------------------------------------------------ */
/* enrolment                                                           */
/* ------------------------------------------------------------------ */
static void enrol_send(void)
{
    if (!s_cli || !s_connected) return;
    cloud_info_t in = { 0 }; if (s_info_fn) s_info_fn(&in);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "proto", 2);
    cJSON_AddStringToObject(j, "device_id", s_devid);
    cJSON_AddStringToObject(j, "model", "perceptio-2");
    char hw[8]; snprintf(hw, sizeof hw, "%d", PERCEPTIO_HW_REV);
    cJSON_AddStringToObject(j, "hw_rev", hw);
    cJSON_AddStringToObject(j, "fw", config_fw_version());
    cJSON *caps = cJSON_AddObjectToObject(j, "caps");
    cJSON_AddNumberToObject(caps, "gyro", 1);
    cJSON *acc = cJSON_AddArrayToObject(caps, "accel");
    cJSON *a0 = cJSON_CreateObject(); cJSON_AddNumberToObject(a0, "fsr_g", in.xl_fs_g); cJSON_AddItemToArray(acc, a0);
    cJSON *a1 = cJSON_CreateObject(); cJSON_AddNumberToObject(a1, "fsr_g", in.hg_fs_g); cJSON_AddItemToArray(acc, a1);
    cJSON *rh = cJSON_AddArrayToObject(caps, "rate_hz"); cJSON_AddItemToArray(rh, cJSON_CreateNumber(in.odr_hz));
    cJSON_AddStringToObject(j, "nonce", s_nonce);
    char t[64]; snprintf(t, sizeof t, "perceptio/enrol/%s/request", s_devid);
    pub_json(t, j, 1, false);
    s_last_enrol_tx = esp_timer_get_time();
    ESP_LOGI(TAG, "enrolment request sent (approve it at admin.padelsense.net/p2/)");
}

static void enrol_response(const char *payload, int len)
{
    cJSON *j = cJSON_ParseWithLength(payload, len);
    const cJSON *nonce = j ? cJSON_GetObjectItem(j, "nonce") : NULL;
    const cJSON *status = j ? cJSON_GetObjectItem(j, "status") : NULL;
    if (!cJSON_IsString(nonce) || strcmp(nonce->valuestring, s_nonce) != 0) { ESP_LOGW(TAG, "enrol: response with another nonce — ignored"); cJSON_Delete(j); return; }
    if (cJSON_IsString(status) && strcmp(status->valuestring, "approved") == 0) {
        const cJSON *u = cJSON_GetObjectItem(j, "username"), *p = cJSON_GetObjectItem(j, "password");
        if (cJSON_IsString(u) && cJSON_IsString(p)) {
            save_credential(u->valuestring, p->valuestring);
            ESP_LOGI(TAG, "enrolment APPROVED — switching to the racket login");
            xTaskNotify(s_task, 1, eSetBits);                /* reconnect with the new credential, from the cloud task */
        }
    } else {
        ESP_LOGW(TAG, "enrolment REJECTED — retry in 10 min");
        s_state = CLOUD_ENROL_REJECTED; s_rejected_at = esp_timer_get_time();
    }
    cJSON_Delete(j);
}

/* ------------------------------------------------------------------ */
/* MQTT client                                                         */
/* ------------------------------------------------------------------ */
static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true; s_refusals = 0;
        if (s_enrolled) {
            s_state = CLOUD_ONLINE;
            ESP_LOGI(TAG, "connected as %s", s_user);
            esp_mqtt_client_subscribe(s_cli, topic("cmd"), 1);
            publish_hello();
            publish_state();
            ev_replay();
        } else {
            s_state = CLOUD_ENROL_WAIT;
            char t[64]; snprintf(t, sizeof t, "perceptio/enrol/%s/response", s_devid);
            esp_mqtt_client_subscribe(s_cli, t, 1);
            esp_fill_random(s_nonce, 16);
            for (int i = 15; i >= 0; i--) snprintf(s_nonce + 2 * i, 3, "%02x", (uint8_t)s_nonce[i]);
            enrol_send();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        if (s_state == CLOUD_ONLINE) s_state = CLOUD_OFFLINE;
        /* esp-mqtt keeps every unacknowledged QoS 1 publish in its outbox and resends ALL of them on reconnect, on top
         * of our own hello/state/replay. After a few failed connects that burst exceeds the broker's 20 in-flight limit
         * and the loop never ends. So: no auto-reconnect; the cloud task destroys and recreates the client (empty
         * outbox) after a pause. Our event ring is the source of truth for what still has to be sent. */
        s_reconnect_due = esp_timer_get_time() + RECONNECT_MS * 1000LL;
        if (s_task) xTaskNotify(s_task, 2, eSetBits);
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len > 0 && strncmp(ev->topic, "perceptio/enrol/", 16) == 0) enrol_response(ev->data, ev->data_len);
        else if (ev->topic_len > 4 && strncmp(ev->topic + ev->topic_len - 4, "/cmd", 4) == 0) handle_cmd(ev->data, ev->data_len);
        break;
    case MQTT_EVENT_PUBLISHED:
        ev_mark_sent(ev->msg_id);
        if (s_puback && ev->msg_id == s_wait_msg_id) xSemaphoreGive(s_puback);
        if (s_task) xTaskNotify(s_task, 2, eSetBits);          /* window freed: the cloud task sends the next ones */
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "connection refused (code %d)", ev->error_handle->connect_return_code);
            if (s_enrolled && ++s_refusals >= 3) {
                ESP_LOGW(TAG, "refused 3x — credential revoked? back to enrolment");
                save_credential(NULL, NULL);
                xTaskNotify(s_task, 1, eSetBits);
            }
        } else if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error (tls/socket %d)", ev->error_handle->esp_tls_last_esp_err);
        }
        break;
    default: break;
    }
}

static void client_start(void)
{
    if (s_cli) { esp_mqtt_client_stop(s_cli); esp_mqtt_client_destroy(s_cli); s_cli = NULL; s_connected = false; }
    static char will[32] = "{\"online\":false}";
    esp_mqtt_client_config_t c = {
        .broker.address.uri = BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s_devid,
        .credentials.username = s_enrolled ? s_user : "enrol",
        .credentials.authentication.password = s_enrolled ? s_pass : ENROL_PASSWORD,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .session.keepalive = 30,
        .session.disable_clean_session = s_enrolled,          /* persistent session once enrolled */
        .network.disable_auto_reconnect = true,              /* the cloud task recreates the client: see on_mqtt DISCONNECTED */
        .network.timeout_ms = 10000,
        .task.priority = 5,
        .task.stack_size = 7168,
        .buffer.size = 4096,
        .buffer.out_size = 8192,                             /* hello with the inventory can exceed 4 KB */
    };
    if (s_enrolled) {
        c.session.last_will.topic = topic("state");
        c.session.last_will.msg = will; c.session.last_will.msg_len = strlen(will);
        c.session.last_will.qos = 1; c.session.last_will.retain = true;
    }
    s_reconnect_due = 0;
    s_cli = esp_mqtt_client_init(&c);
    esp_mqtt5_connection_property_config_t p5 = { .session_expiry_interval = s_enrolled ? 3600 : 0 };
    esp_mqtt5_client_set_connect_property(s_cli, &p5);
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    s_state = CLOUD_CONNECTING;
    esp_mqtt_client_start(s_cli);
    ESP_LOGI(TAG, "MQTT client started: %s as '%s' (%s)", BROKER_URI, s_enrolled ? s_user : "enrol", s_enrolled ? "racket" : "enrolment");
}

/* ------------------------------------------------------------------ */
/* cloud task (core 0): timers + mode switches                         */
/* ------------------------------------------------------------------ */
/* update mode (cloud_suspend/resume): done by the cloud task itself, the only task that drives s_cli */
static volatile bool     s_suspended;
static const char       *s_susp_reason;
static SemaphoreHandle_t s_susp_done;

static void task(void *arg)
{
    for (;;) {
        uint32_t notif = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &notif, pdMS_TO_TICKS(1000));
        if (notif & 4) {                                     /* suspend: announce, then free the client */
            s_suspended = true;                              /* first: nothing below may recreate it */
            cloud_shutdown(s_susp_reason ? s_susp_reason : "suspend");   /* ≤ 1.2 s, only if connected */
            if (s_cli) { esp_mqtt_client_stop(s_cli); esp_mqtt_client_destroy(s_cli); s_cli = NULL; }
            s_connected = false; s_reconnect_due = 0; s_state = CLOUD_OFF;
            ESP_LOGW(TAG, "suspended (%s): MQTT client destroyed, %u KB internal free", s_susp_reason ? s_susp_reason : "",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
            if (s_susp_done) xSemaphoreGive(s_susp_done);
        }
        if ((notif & 8) && s_suspended) {                    /* resume */
            s_suspended = false;
            ESP_LOGW(TAG, "resumed%s", s_link_up ? ": reconnecting" : " (no link yet)");
            if (s_link_up) client_start();
        }
        if (s_suspended) continue;
        if (notif & 1) {                                     /* credential changed: reconnect in the right mode */
            if (s_link_up) client_start();
        }
        if (!s_link_up || !s_cli) continue;
        int64_t now = esp_timer_get_time();
        if (!s_connected && s_reconnect_due && now >= s_reconnect_due) { s_reconnect_due = 0; client_start(); continue; }
        if (s_connected && s_enrolled) {
            if (s_hello_dirty) { s_hello_dirty = false; publish_hello(); }
            if (s_state_dirty) { s_state_dirty = false; publish_state(); }
            ack_flush();
            ev_flush();
            {   /* remote log: whole lines, ≤ 4 KB per message, QoS 0 (spec: payload ≤ 4 KB) */
                static char *buf; if (!buf) buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                size_t n = buf ? rlog_take(buf, 2047) : 0;       /* 2 KB/s max: a boot burst drains in a few seconds */
                if (n) { buf[n] = 0; esp_mqtt_client_publish(s_cli, topic("log"), buf, n, 0, 0); }
            }
            if (now - s_last_telemetry > TELEMETRY_MS * 1000LL) { s_last_telemetry = now; publish_telemetry(); }
            if (now - s_last_sntp > SNTP_PERIOD_MS * 1000LL)   { s_last_sntp = now; clock_resync(); }
        } else if (s_connected && !s_enrolled) {
            if (s_state == CLOUD_ENROL_REJECTED) {
                if (now - s_rejected_at > REJECT_RETRY_MS * 1000LL) { s_state = CLOUD_ENROL_WAIT; enrol_send(); }
            } else if (now - s_last_enrol_tx > ENROL_RESEND_MS * 1000LL) enrol_send();
        }
    }
}

/* SNTP callback (lwIP thread): cloud_event only queues, the cloud task publishes. */
static void on_clock_synced(int32_t offset_ms, bool known)
{
    char d[48];
    if (known) snprintf(d, sizeof d, "{\"offset_ms\":%" PRId32 "}", offset_ms);
    else       snprintf(d, sizeof d, "{\"offset_ms\":null}");
    cloud_event("time_synced", d);
}

/* A crash leaves a core dump in flash. Publish its summary ONCE (task, cause, PC, backtrace) and KEEP the dump: it is
 * the evidence (all task stacks, registers) and is fetched with GET /api/v1/coredump, erased with DELETE (or overwritten
 * by the next crash). "Once" = the dump's own CRC32 (last 4 bytes of the image) is remembered in NVS "cd_seen".
 * Until 0.3.22 the dump was erased right after the one-line summary — twice that destroyed the evidence. */
static void crash_report(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return;
    size_t addr = 0, size = 0; uint32_t crc = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size < 8 || esp_flash_read(NULL, &crc, addr + size - 4, 4) != ESP_OK) {
        ESP_LOGE(TAG, "core dump in flash but unreadable: kept, fetch it with GET /api/v1/coredump");
        return;
    }
    uint32_t seen = 0; nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) { nvs_get_u32(h, "cd_seen", &seen); }
    else h = 0;
    if (seen == crc) {
        ESP_LOGW(TAG, "core dump in flash (%u B, crc %08" PRIx32 ", already reported): GET /api/v1/coredump, DELETE to erase", (unsigned)size, crc);
        if (h) nvs_close(h);
        return;
    }
    esp_core_dump_summary_t *sum = calloc(1, sizeof *sum);
    esp_err_t se = sum ? esp_core_dump_get_summary(sum) : ESP_ERR_NO_MEM;
    if (se == ESP_OK) {
        char bt[100] = "";                                   /* ≤ 8 × "0x%08x", fits the 192 B event data with the rest */
        int n = 0;
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 8 && n < (int)sizeof bt - 14; i++)
            n += snprintf(bt + n, sizeof bt - n, "%s\"0x%08" PRIx32 "\"", i ? "," : "", sum->exc_bt_info.bt[i]);
        ESP_LOGE(TAG, "previous boot CRASHED: task '%s' pc 0x%08" PRIx32 " cause %" PRIu32 " vaddr 0x%08" PRIx32 " bt [%s]%s",
                 sum->exc_task, sum->exc_pc, sum->ex_info.exc_cause, sum->ex_info.exc_vaddr, bt, sum->exc_bt_info.corrupted ? " (corrupted)" : "");
        char d[192];
        snprintf(d, sizeof d, "{\"task\":\"%.15s\",\"pc\":\"0x%08" PRIx32 "\",\"cause\":%" PRIu32 ",\"bt\":[%.99s]}",
                 sum->exc_task, sum->exc_pc, sum->ex_info.exc_cause, bt);
        cloud_event("crash", d);
    } else {
        ESP_LOGE(TAG, "previous boot CRASHED: core dump summary unreadable (%s)", esp_err_to_name(se));
        cloud_event("crash", "{\"task\":\"?\"}");
    }
    free(sum);
    ESP_LOGW(TAG, "core dump kept in flash (%u B, crc %08" PRIx32 "): GET /api/v1/coredump, DELETE to erase", (unsigned)size, crc);
    if (h) { nvs_set_u32(h, "cd_seen", crc); nvs_commit(h); nvs_close(h); }
}

/* ------------------------------------------------------------------ */
esp_err_t cloud_init(cloud_info_fn info)
{
    /* the cloud is one listener of the device bus: events/state/inventory announced by any module reach MQTT */
    static const bus_sink_t sink = { .name = "cloud", .on_event = cloud_event, .on_state = cloud_set_state,
                                     .on_inventory = cloud_inventory_changed };
    bus_add_sink(&sink);
    s_info_fn = info;
    clock_set_sync_hook(on_clock_synced);
    load_identity();
    s_ackq = heap_caps_calloc(ACK_Q, sizeof(ack_q_t), MALLOC_CAP_SPIRAM);
    s_ev = heap_caps_calloc(EV_RING, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    if (!s_ev) s_ev = calloc(EV_RING, sizeof(ev_t));
    s_state_since_ms = clock_utc_ms();
    xTaskCreatePinnedToCore(task, "cloud", 6144, NULL, 4, &s_task, 0);
    char d[48]; snprintf(d, sizeof d, "{\"reset_reason\":\"%s\"}", reset_reason_name());
    cloud_event("boot", d);                                   /* buffered until the first hello */
    crash_report();
    ESP_LOGI(TAG, "device_id %s, boot_id %" PRIu32 ", %s, reset %s", s_devid, s_boot_id, s_enrolled ? "enrolled" : "NOT enrolled", reset_reason_name());
    return ESP_OK;
}

void cloud_suspend(const char *reason)
{
    if (!s_task) return;
    if (!s_susp_done) s_susp_done = xSemaphoreCreateBinary();
    if (!s_susp_done) return;
    s_susp_reason = reason;
    xSemaphoreTake(s_susp_done, 0);
    xTaskNotify(s_task, 4, eSetBits);
    if (xSemaphoreTake(s_susp_done, pdMS_TO_TICKS(12000)) != pdTRUE)   /* stop waits for an MQTT connect in progress */
        ESP_LOGW(TAG, "suspend (%s): cloud task did not confirm in 12 s", reason);
}
void cloud_resume(void) { if (s_task) xTaskNotify(s_task, 8, eSetBits); }

void cloud_link(bool up)
{
    bool was = s_link_up;
    s_link_up = up;
    if (up && !was) {
        cloud_event("wifi_back", NULL);
        if (!s_suspended) client_start();                     /* fresh client (empty outbox) on every link-up */
    } else if (!up && was) {
        cloud_event("wifi_lost", NULL);
        s_connected = false;
        if (s_state == CLOUD_ONLINE) s_state = CLOUD_OFFLINE;
    }
}

void cloud_shutdown(const char *reason)
{
    if (!s_cli || !s_connected || !s_enrolled) { ESP_LOGW(TAG, "shutdown (%s): offline, nothing to tell the backend", reason); return; }
    if (!s_puback) s_puback = xSemaphoreCreateBinary();
    char d[48]; snprintf(d, sizeof d, "{\"by\":\"%s\"}", reason);
    cloud_event("shutdown", d);                               /* QoS 1, ordered before the state message */

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "online", false);
    cJSON_AddStringToObject(j, "state", "off");
    cJSON_AddStringToObject(j, "reason", reason);
    cJSON_AddNumberToObject(j, "since_utc_ms", (double)clock_utc_ms());
    cJSON_AddNumberToObject(j, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(j, "seq", s_seq);
    xSemaphoreTake(s_puback, 0);
    s_wait_msg_id = pub_json(topic("state"), j, 1, true);     /* retained: replaces the "online" state until the next hello */
    bool acked = s_wait_msg_id > 0 && xSemaphoreTake(s_puback, pdMS_TO_TICKS(1200)) == pdTRUE;
    ESP_LOGI(TAG, "shutdown (%s) announced%s", reason, acked ? "" : " — no PUBACK in time");
    esp_mqtt_client_disconnect(s_cli);                       /* clean DISCONNECT: the broker will not publish the will */
    s_connected = false; s_state = CLOUD_OFF;
}

void cloud_upload_done(const char *cmd_id, bool ok, const char *reason, const char *result_json, const char *stats_json)
{
    cJSON *r = (ok && result_json) ? cJSON_Parse(result_json) : NULL;
    if (!r) r = cJSON_CreateObject();
    cJSON *st = stats_json ? cJSON_Parse(stats_json) : NULL;        /* firmware-upload-speed.md §1: timing in the ack */
    if (st) cJSON_AddItemToObject(r, "stats", st);
    hist_put(cmd_id, ok ? "applied" : "rejected", ok ? NULL : reason);
    ack_ex(cmd_id, ok ? "applied" : "rejected", ok ? NULL : reason, r, true);   /* from the upload task: queued, sent by the cloud task */
    cloud_inventory_changed();
}

void cloud_inventory_changed(void)
{
    s_hello_dirty = true;
    if (s_task) xTaskNotify(s_task, 2, eSetBits);
}

void cloud_set_state(const char *state)
{
    if (strcmp(s_dev_state, state) == 0) return;
    strncpy(s_dev_state, state, sizeof s_dev_state - 1);
    s_state_since_ms = clock_utc_ms();
    s_state_dirty = true;                               /* published by the cloud task (see cloud_event) */
    if (s_task) xTaskNotify(s_task, 2, eSetBits);
}

void cloud_get_status(cloud_status_t *st)
{
    *st = s_status;
    st->state = s_state; st->enrolled = s_enrolled;
    strncpy(st->device_id, s_devid, sizeof st->device_id);
    st->boot_id = s_boot_id;
    st->events_pending = s_ev ? ev_pending() : 0;
}

const char *cloud_device_id(void) { return s_devid; }
const char *cloud_state_name(cloud_state_t s)
{
    static const char *n[] = { "off", "waiting for approval", "rejected", "connecting", "online", "offline" };
    return n[s <= CLOUD_OFFLINE ? s : 0];
}
