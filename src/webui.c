#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "webui.h"
#include "config.h"
#include "selftest.h"
#include "upload.h"
#include "power.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "task_psram.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "cJSON.h"

static const char *TAG = "WEBUI";
static webui_status_fn s_status_fn;
static httpd_handle_t  s_hd;

/* embedded, gzipped web app (web/index.html -> web/index.html.gz by tools/gzip_web.py) */
extern const uint8_t index_html_gz[];
extern const size_t  index_html_gz_len;

#ifndef PERCEPTIO_HW_REV
#define PERCEPTIO_HW_REV 0
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static void cors(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t send_json(httpd_req_t *r, cJSON *j, const char *status)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(r);
    cors(r);
    if (status) httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return e;
}

static esp_err_t send_err(httpd_req_t *r, const char *status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(r, j, status);
}

static cJSON *read_body(httpd_req_t *r)
{
    char buf[512];
    if (r->content_len == 0 || r->content_len >= sizeof buf) return NULL;
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) return NULL;
        got += n;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o = (char)strtol(h, NULL, 16); s += 2; }
        else if (*s == '+') *o = ' ';
        else *o = *s;
    }
    *o = 0;
}

/* ------------------------------------------------------------------ */
/* static page                                                         */
/* ------------------------------------------------------------------ */
static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");   /* the page changes with every firmware; never serve a stale copy */
    return httpd_resp_send(r, (const char *)index_html_gz, index_html_gz_len);
}

static esp_err_t h_options(httpd_req_t *r)      /* CORS preflight for PATCH/DELETE from other origins */
{
    cors(r);
    httpd_resp_set_hdr(r, "Access-Control-Max-Age", "600");
    return httpd_resp_send(r, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* /api/v1/info — static identity                                      */
/* ------------------------------------------------------------------ */
static esp_err_t h_info(httpd_req_t *r)
{
    webui_status_t st = { 0 };
    if (s_status_fn) s_status_fn(&st);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18]; snprintf(macs, sizeof macs, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_chip_info_t ci; esp_chip_info(&ci);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", g_cfg.dev_name);
    cJSON_AddStringToObject(j, "model", "Perceptio 2.0");
    cJSON_AddStringToObject(j, "fw", config_fw_version());
    cJSON_AddNumberToObject(j, "hw", PERCEPTIO_HW_REV);
    cJSON_AddStringToObject(j, "api", "1");
    cJSON_AddStringToObject(j, "mac", macs);
    cJSON_AddStringToObject(j, "device_id", cloud_device_id());
    char qr[20]; snprintf(qr, sizeof qr, "P2:%s", cloud_device_id());
    cJSON_AddStringToObject(j, "qr", qr);
    cJSON_AddStringToObject(j, "ip", st.ip);
    cJSON_AddStringToObject(j, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(j, "chip_rev", ci.revision);
    cJSON_AddNumberToObject(j, "odr_hz", st.odr_hz);
    cJSON_AddNumberToObject(j, "xl_fs_g", st.xl_fs_g);
    cJSON_AddNumberToObject(j, "gy_fs_dps", st.gy_fs_dps);
    cJSON_AddNumberToObject(j, "hg_fs_g", st.hg_fs_g);
    cJSON_AddNumberToObject(j, "xl_mg_lsb", st.xl_mg_lsb);
    cJSON_AddNumberToObject(j, "gy_mdps_lsb", st.gy_mdps_lsb);
    cJSON_AddNumberToObject(j, "hg_mg_lsb", st.hg_mg_lsb);
    cJSON_AddNumberToObject(j, "sample_bytes", (int)sizeof(webui_sample_t));
    return send_json(r, j, NULL);
}

/* ------------------------------------------------------------------ */
/* /api/v1/status — live numbers                                       */
/* ------------------------------------------------------------------ */
static esp_err_t h_status(httpd_req_t *r)
{
    webui_status_t st = { 0 };
    if (s_status_fn) s_status_fn(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uptime", st.uptime_s);
    cJSON_AddNumberToObject(j, "heap_free", st.heap_free);
    cJSON_AddNumberToObject(j, "heap_min", st.heap_min);
    cJSON_AddNumberToObject(j, "vbat", st.vbat);
    cJSON_AddBoolToObject  (j, "vbat_cal", st.vbat_cal);
    cJSON_AddNumberToObject(j, "soc_pct", st.soc_pct);
    cJSON_AddBoolToObject  (j, "vin", st.bq_stat0 & 1);
    cJSON_AddNumberToObject(j, "chg", (st.bq_stat0 >> 5) & 3);
    cJSON_AddNumberToObject(j, "rssi", st.rssi);
    cJSON_AddStringToObject(j, "ip", st.ip);
    cJSON_AddNumberToObject(j, "imu_hz", st.imu_rate_hz);
    cJSON_AddNumberToObject(j, "imu_total", st.imu_total);
    cJSON_AddNumberToObject(j, "imu_missed", st.imu_missed);
    cJSON_AddNumberToObject(j, "imu_temp", st.imu_temp_c);
    cJSON *a = cJSON_AddArrayToObject(j, "accel_mg"); cJSON_AddItemToArray(a, cJSON_CreateNumber(st.ax)); cJSON_AddItemToArray(a, cJSON_CreateNumber(st.ay)); cJSON_AddItemToArray(a, cJSON_CreateNumber(st.az));
    cJSON *g = cJSON_AddArrayToObject(j, "gyro_dps"); cJSON_AddItemToArray(g, cJSON_CreateNumber(st.gx)); cJSON_AddItemToArray(g, cJSON_CreateNumber(st.gy)); cJSON_AddItemToArray(g, cJSON_CreateNumber(st.gz));
    cJSON *hg = cJSON_AddArrayToObject(j, "hg_mg"); cJSON_AddItemToArray(hg, cJSON_CreateNumber(st.hx)); cJSON_AddItemToArray(hg, cJSON_CreateNumber(st.hy)); cJSON_AddItemToArray(hg, cJSON_CreateNumber(st.hz));
    cJSON_AddNumberToObject(j, "hg_peak_g", st.hg_peak_g);
    cJSON *t = cJSON_AddObjectToObject(j, "time");
    cJSON_AddBoolToObject  (t, "valid", st.clk.valid);
    cJSON_AddNumberToObject(t, "epoch", (double)st.clk.now);
    cJSON_AddStringToObject(t, "local", st.clk.local);
    cJSON_AddStringToObject(t, "tz", st.clk.tz);
    cJSON_AddStringToObject(t, "source", clock_source_name(st.clk.source));
    cJSON_AddNumberToObject(t, "last_sync", (double)st.clk.last_sync);
    cJSON_AddNumberToObject(t, "age_s", st.clk.age_s);
    cJSON *cl = cJSON_AddObjectToObject(j, "cloud");
    cJSON_AddStringToObject(cl, "state", cloud_state_name(st.cloud.state));
    cJSON_AddBoolToObject  (cl, "enrolled", st.cloud.enrolled);
    cJSON_AddStringToObject(cl, "device_id", st.cloud.device_id);
    cJSON_AddNumberToObject(cl, "boot_id", st.cloud.boot_id);
    cJSON_AddNumberToObject(cl, "events_pending", st.cloud.events_pending);
    cJSON_AddNumberToObject(cl, "last_hello_utc_ms", (double)st.cloud.last_hello_utc_ms);
    cJSON_AddNumberToObject(cl, "cmds", st.cloud.cmds_received);
    cJSON_AddStringToObject(cl, "last_cmd", st.cloud.last_cmd);
    cJSON *rc = cJSON_AddObjectToObject(j, "rec");
    cJSON_AddStringToObject(rc, "state", store_state_name(st.store.state));
    if (st.store.state != REC_IDLE) {
        cJSON_AddStringToObject(rc, "rec_id", st.store.cur.rec_id);
        cJSON_AddStringToObject(rc, "rec_tag", st.store.cur.rec_tag);
        cJSON_AddNumberToObject(rc, "elapsed_ms", st.store.elapsed_ms);
        cJSON_AddNumberToObject(rc, "samples", st.store.cur.samples);
        cJSON_AddNumberToObject(rc, "dropped", st.store.cur.dropped);
        cJSON_AddNumberToObject(rc, "pages", st.store.cur.pages);
        cJSON_AddNumberToObject(rc, "backlog_max", st.store.backlog_max);
    }
    { upload_status_t us; upload_get_status(&us);
      cJSON *up = cJSON_AddObjectToObject(j, "upload");
      static const char *ph[] = { "idle", "resuming", "sending", "verifying", "backoff" };
      cJSON_AddStringToObject(up, "phase", ph[us.phase <= UP_BACKOFF ? us.phase : 0]);
      if (us.phase != UP_IDLE) {
          cJSON_AddStringToObject(up, "rec_id", us.rec_id);
          cJSON_AddNumberToObject(up, "sent_bytes", us.sent_bytes);
          cJSON_AddNumberToObject(up, "total_bytes", us.total_bytes);
          cJSON_AddNumberToObject(up, "retries", us.retries);
          cJSON_AddStringToObject(up, "last_error", us.last_error);
      } }
    cJSON_AddNumberToObject(rc, "count", st.store.count);
    cJSON_AddNumberToObject(rc, "total_mb", st.store.total_mb);
    cJSON_AddNumberToObject(rc, "free_mb", st.store.free_mb);
    cJSON_AddNumberToObject(j, "tests_pass", st.tests_pass);
    cJSON_AddNumberToObject(j, "tests_total", st.tests_total);
    return send_json(r, j, NULL);
}

/* ------------------------------------------------------------------ */
/* /api/v1/schema + /api/v1/config — the editable parameters           */
/* ------------------------------------------------------------------ */
static esp_err_t h_schema(httpd_req_t *r)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < config_param_count(); i++) {
        const cfg_param_t *p = config_param(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", p->key);
        cJSON_AddStringToObject(o, "label", p->label);
        cJSON_AddStringToObject(o, "group", p->group);
        cJSON_AddStringToObject(o, "unit", p->unit);
        cJSON_AddStringToObject(o, "type", p->type == P_INT ? "int" : p->type == P_BOOL ? "bool" : "str");
        cJSON_AddNumberToObject(o, "min", p->min);
        cJSON_AddNumberToObject(o, "max", p->max);
        if (p->options && *p->options) {
            cJSON *opt = cJSON_AddArrayToObject(o, "options");
            const char *s = p->options;
            while (*s) { cJSON_AddItemToArray(opt, cJSON_CreateNumber(strtol(s, (char **)&s, 10))); if (*s == ',') s++; }
        }
        cJSON_AddBoolToObject(o, "reboot", p->reboot);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(r, a, NULL);
}

static cJSON *config_to_json(void)
{
    cJSON *j = cJSON_CreateObject();
    for (int i = 0; i < config_param_count(); i++) {
        const cfg_param_t *p = config_param(i);
        switch (p->type) {
        case P_INT:  cJSON_AddNumberToObject(j, p->key, *(int32_t *)p->ptr); break;
        case P_BOOL: cJSON_AddBoolToObject  (j, p->key, *(bool *)p->ptr);    break;
        case P_STR:  cJSON_AddStringToObject(j, p->key, (char *)p->ptr);     break;
        }
    }
    return j;
}

static esp_err_t h_config_get(httpd_req_t *r) { return send_json(r, config_to_json(), NULL); }

static esp_err_t h_config_patch(httpd_req_t *r)
{
    if (config_is_locked()) return send_err(r, "409 Conflict", "recording in progress: parameters are locked");
    cJSON *b = read_body(r);
    if (!b || !cJSON_IsObject(b)) { cJSON_Delete(b); return send_err(r, "400 Bad Request", "JSON object expected"); }
    cJSON *errors = cJSON_CreateObject();
    int applied = 0;
    for (cJSON *it = b->child; it; it = it->next) {
        const cfg_param_t *p = config_param_find(it->string);
        esp_err_t e;
        if (!p) e = ESP_ERR_NOT_FOUND;
        else if (p->type == P_INT  && cJSON_IsNumber(it)) e = config_param_set_int(p->key, (int32_t)it->valuedouble);
        else if (p->type == P_BOOL && cJSON_IsBool(it))   e = config_param_set_bool(p->key, cJSON_IsTrue(it));
        else if (p->type == P_STR  && cJSON_IsString(it)) e = config_param_set_str(p->key, it->valuestring);
        else e = ESP_ERR_INVALID_ARG;
        if (e == ESP_OK) applied++;
        else cJSON_AddStringToObject(errors, it->string, e == ESP_ERR_NOT_FOUND ? "unknown parameter" : e == ESP_ERR_INVALID_ARG ? "invalid value or type" : esp_err_to_name(e));
    }
    cJSON_Delete(b);
    cJSON *resp = config_to_json();
    cJSON_AddNumberToObject(resp, "applied", applied);
    cJSON_AddItemToObject(resp, "errors", errors);
    return send_json(r, resp, applied || !errors->child ? NULL : "400 Bad Request");
}

/* ------------------------------------------------------------------ */
/* /api/v1/wifi                                                        */
/* ------------------------------------------------------------------ */
static esp_err_t h_wifi_get(httpd_req_t *r)
{
    const cfg_wifi_list_t *l = config_wifi_get();
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < l->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", l->list[i].ssid);
        cJSON_AddBoolToObject(o, "open", l->list[i].pass[0] == 0);    /* passwords never leave the device */
        cJSON_AddItemToArray(a, o);
    }
    return send_json(r, a, NULL);
}

static esp_err_t h_wifi_post(httpd_req_t *r)
{
    if (config_is_locked()) return send_err(r, "409 Conflict", "recording in progress: settings are locked");
    cJSON *b = read_body(r);
    const cJSON *ssid = b ? cJSON_GetObjectItem(b, "ssid") : NULL;
    const cJSON *pass = b ? cJSON_GetObjectItem(b, "pass") : NULL;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid)) {
        e = config_wifi_add(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
        if (e == ESP_OK) e = config_wifi_save();         /* NVS write — not allowed during a capture session */
    }
    cJSON_Delete(b);
    if (e != ESP_OK) return send_err(r, "400 Bad Request", esp_err_to_name(e));
    return h_wifi_get(r);
}

static esp_err_t h_wifi_delete(httpd_req_t *r)          /* DELETE /api/v1/wifi/<urlencoded ssid> */
{
    if (config_is_locked()) return send_err(r, "409 Conflict", "recording in progress: settings are locked");
    char ssid[CFG_SSID_LEN * 3 + 1];
    const char *p = r->uri + strlen("/api/v1/wifi/");
    strncpy(ssid, p, sizeof ssid - 1); ssid[sizeof ssid - 1] = 0;
    url_decode(ssid);
    esp_err_t e = config_wifi_remove(ssid);
    if (e == ESP_OK) e = config_wifi_save();
    if (e != ESP_OK) return send_err(r, e == ESP_ERR_NOT_FOUND ? "404 Not Found" : "400 Bad Request", esp_err_to_name(e));
    return h_wifi_get(r);
}

/* POST /api/v1/time {"epoch": 1760000000}  — the app sets the clock when there is no internet */
static esp_err_t h_time_post(httpd_req_t *r)
{
    cJSON *b = read_body(r);
    const cJSON *ep = b ? cJSON_GetObjectItem(b, "epoch") : NULL;
    esp_err_t e = cJSON_IsNumber(ep) ? clock_set_epoch((time_t)ep->valuedouble, CLK_SRC_APP) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(b);
    if (e != ESP_OK) return send_err(r, "400 Bad Request", "epoch (seconds since 1970, >= 2024) required");
    clock_status_t cs; clock_get_status(&cs);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "epoch", (double)cs.now);
    cJSON_AddStringToObject(j, "local", cs.local);
    return send_json(r, j, NULL);
}

/* ------------------------------------------------------------------ */
static esp_err_t h_selftest(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "passed", selftest_passed());
    cJSON_AddNumberToObject(j, "total", selftest_count());
    cJSON *a = cJSON_AddArrayToObject(j, "results");
    for (int i = 0; i < selftest_count(); i++) {
        const selftest_result_t *t = selftest_result(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddBoolToObject(o, "pass", t->pass);
        cJSON_AddStringToObject(o, "info", t->info);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(r, j, NULL);
}

/* ------------------------------------------------------------------ */
static esp_err_t h_reboot(httpd_req_t *r)
{
    cors(r);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* POST /api/v1/power/off — same as the 5 s button press. Answers first: with a cable in the device reboots into
 * CHARGE-IDLE (UI gone), on battery it enters ship mode. */
static esp_err_t h_power_off(httpd_req_t *r)
{
    cors(r);
    bq25180_status_t bq; power_charger_status(&bq);
    char out[96];
    snprintf(out, sizeof out, "{\"ok\":true,\"mode\":\"%s\",\"recording_closed\":%s}",
             bq.vin_pgood ? "charge_idle" : "ship", store_is_recording() ? "true" : "false");
    httpd_resp_sendstr(r, out);
    app_request_power_off("ui");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* /api/v1/recordings — the NAND store                                 */
/* ------------------------------------------------------------------ */
static cJSON *rec_json(const store_rec_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "rec_id", r->rec_id);
    cJSON_AddStringToObject(o, "rec_tag", r->rec_tag);
    if (r->start_utc_ms) cJSON_AddNumberToObject(o, "started_utc_ms", (double)r->start_utc_ms); else cJSON_AddNullToObject(o, "started_utc_ms");
    cJSON_AddNumberToObject(o, "duration_ms", r->duration_ms);
    cJSON_AddNumberToObject(o, "samples", r->samples);
    cJSON_AddNumberToObject(o, "dropped", r->dropped);
    cJSON_AddNumberToObject(o, "saturated", r->saturated);
    cJSON_AddNumberToObject(o, "blocks", r->blocks);
    cJSON_AddNumberToObject(o, "bytes", (double)r->bytes);
    cJSON_AddNumberToObject(o, "uploaded_bytes", r->uploaded_bytes);
    cJSON_AddNumberToObject(o, "rate_hz", r->odr_hz);
    cJSON_AddBoolToObject  (o, "complete", r->complete);
    cJSON_AddBoolToObject  (o, "this_boot", r->this_boot);
    return o;
}

static esp_err_t h_rec_list(httpd_req_t *r)
{
    store_status_t ss; store_get_status(&ss);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", store_state_name(ss.state));
    cJSON_AddBoolToObject  (j, "nand_ok", ss.nand_ok);
    if (ss.state != REC_IDLE) { cJSON *c = rec_json(&ss.cur); cJSON_AddNumberToObject(c, "elapsed_ms", ss.elapsed_ms); cJSON_AddItemToObject(j, "current", c); }
    cJSON_AddNumberToObject(j, "total_mb", ss.total_mb);
    cJSON_AddNumberToObject(j, "free_mb", ss.free_mb);
    cJSON *a = cJSON_AddArrayToObject(j, "recordings");
    for (int i = 0; i < store_count(); i++) { const store_rec_t *x = store_rec(i); if (x) cJSON_AddItemToArray(a, rec_json(x)); }
    return send_json(r, j, NULL);
}

static esp_err_t h_rec_start(httpd_req_t *r)         /* POST {"rec_tag": "..."} (optional) */
{
    cJSON *b = read_body(r);
    const cJSON *tag = b ? cJSON_GetObjectItem(b, "rec_tag") : NULL;
    char tagbuf[65] = "";
    if (cJSON_IsString(tag)) strncpy(tagbuf, tag->valuestring, sizeof tagbuf - 1);
    cJSON_Delete(b);
    char rid[32] = "";
    esp_err_t e = store_start(tagbuf, "ui", rid);
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "already recording");
    if (e == ESP_ERR_NO_MEM)        return send_err(r, "507 Insufficient Storage", "storage full");
    if (e != ESP_OK)                return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "rec_id", rid);
    return send_json(r, j, NULL);
}

static esp_err_t h_rec_synth(httpd_req_t *r)         /* POST {"minutes": 150, "rec_tag": "..."} — test aid */
{
    cJSON *b = read_body(r);
    const cJSON *m = b ? cJSON_GetObjectItem(b, "minutes") : NULL;
    const cJSON *tag = b ? cJSON_GetObjectItem(b, "rec_tag") : NULL;
    uint32_t minutes = cJSON_IsNumber(m) ? (uint32_t)m->valuedouble : 150;
    char tagbuf[65] = "";
    if (cJSON_IsString(tag)) strncpy(tagbuf, tag->valuestring, sizeof tagbuf - 1);
    cJSON_Delete(b);
    esp_err_t e = store_generate(minutes, tagbuf);
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "already recording");
    if (e == ESP_ERR_NO_MEM)        return send_err(r, "507 Insufficient Storage", "storage full");
    if (e != ESP_OK)                return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    cJSON *j = cJSON_CreateObject(); cJSON_AddNumberToObject(j, "minutes", minutes);
    return send_json(r, j, NULL);
}

static esp_err_t h_rec_stop(httpd_req_t *r)
{
    store_rec_t done;
    esp_err_t e = store_stop("ui", &done);
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "not recording");
    return send_json(r, rec_json(&done), NULL);
}

/* GET /api/v1/recordings/<rec_id> -> the raw pages (application/octet-stream, tools/p2rec.py parses it)
 * DELETE /api/v1/recordings/<rec_id> */
static void rec_id_from_uri(httpd_req_t *r, char *out, size_t n)
{
    const char *p = r->uri + strlen("/api/v1/recordings/");
    size_t i = 0;
    while (*p && *p != '/' && *p != '?' && i < n - 1) out[i++] = *p++;
    out[i] = 0;
}

static esp_err_t h_rec_data(httpd_req_t *r)          /* the PCR2 file, byte for byte as the upload sends it */
{
    char rid[32]; rec_id_from_uri(r, rid, sizeof rid);
    const store_rec_t *x = store_find(rid);
    if (!x) return send_err(r, "404 Not Found", "unknown recording");
    if (store_is_recording()) return send_err(r, "409 Conflict", "recording in progress");
    static uint8_t *buf;
    if (!buf) buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_err(r, "500 Internal Server Error", "no memory");
    cors(r);
    httpd_resp_set_type(r, "application/octet-stream");
    char cd[64]; snprintf(cd, sizeof cd, "attachment; filename=\"%s.pcr2\"", rid);
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    for (uint32_t off = 0; off < x->bytes;) {
        int n = store_read(rid, off, buf, 4096);
        if (n <= 0) { httpd_resp_send_chunk(r, NULL, 0); return n < 0 ? ESP_FAIL : ESP_OK; }
        if (httpd_resp_send_chunk(r, (const char *)buf, n) != ESP_OK) return ESP_FAIL;
        off += n;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_rec_delete(httpd_req_t *r)
{
    char rid[32]; rec_id_from_uri(r, rid, sizeof rid);
    esp_err_t e = store_delete(rid);
    if (e == ESP_ERR_NOT_FOUND)     return send_err(r, "404 Not Found", "unknown recording");
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "recording in progress");
    cloud_inventory_changed();
    return h_rec_list(r);
}

/* ------------------------------------------------------------------ */
/* live stream: core-1 producer -> ring -> core-0 WebSocket sender     */
/* ------------------------------------------------------------------ */
#define RING_N   4096
#define WS_MAX   4
static webui_sample_t s_ring[RING_N];
static volatile uint32_t s_wr = 0;
static int s_ws_fd[WS_MAX];
static volatile int s_stream_ms = 16;

void webui_feed(const imu_sample_t *smp, int64_t t_us)
{
    (void)t_us;
    uint32_t w = s_wr;
    s_ring[w % RING_N] = *smp;
    __atomic_store_n(&s_wr, w + 1, __ATOMIC_RELEASE);
}

void webui_set_stream_hz(int hz) { if (hz < 10) hz = 10; if (hz > 60) hz = 60; s_stream_ms = 1000 / hz; }

static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(r);
        for (int i = 0; i < WS_MAX; i++) if (s_ws_fd[i] == fd) return ESP_OK;
        for (int i = 0; i < WS_MAX; i++) if (s_ws_fd[i] < 0) { s_ws_fd[i] = fd; ESP_LOGI(TAG, "ws client fd %d", fd); return ESP_OK; }
        return ESP_FAIL;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t e = httpd_ws_recv_frame(r, &f, 0);
    if (e != ESP_OK) return e;
    if (f.len) {                                  /* drain anything the client sends */
        uint8_t *buf = malloc(f.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        f.payload = buf; e = httpd_ws_recv_frame(r, &f, f.len); free(buf);
    }
    return e;
}

static void on_close(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < WS_MAX; i++) if (s_ws_fd[i] == fd) { s_ws_fd[i] = -1; ESP_LOGI(TAG, "ws client fd %d closed", fd); }
    close(fd);
}

static void ws_sender_task(void *arg)
{
    static uint8_t frame[4 + 64 * sizeof(webui_sample_t)];
    uint32_t rd = s_wr;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_stream_ms));
        bool any = false;
        for (int i = 0; i < WS_MAX; i++) if (s_ws_fd[i] >= 0) any = true;
        uint32_t wr = __atomic_load_n(&s_wr, __ATOMIC_ACQUIRE);
        if (!any) { rd = wr; continue; }
        if (wr - rd > RING_N) rd = wr - RING_N;
        while (rd != wr) {
            uint32_t n = wr - rd; if (n > 64) n = 64;
            memcpy(frame, &rd, 4);
            for (uint32_t k = 0; k < n; k++) memcpy(frame + 4 + k * sizeof(webui_sample_t), &s_ring[(rd + k) % RING_N], sizeof(webui_sample_t));
            httpd_ws_frame_t f = { .final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = frame, .len = 4 + n * sizeof(webui_sample_t) };
            for (int i = 0; i < WS_MAX; i++) {
                if (s_ws_fd[i] < 0) continue;
                if (httpd_ws_send_frame_async(s_hd, s_ws_fd[i], &f) != ESP_OK) { ESP_LOGI(TAG, "ws client fd %d gone", s_ws_fd[i]); s_ws_fd[i] = -1; }
            }
            rd += n;
        }
    }
}

/* ------------------------------------------------------------------ */
esp_err_t webui_start(webui_status_fn fn)
{
    s_status_fn = fn;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id          = 0;                      /* UI core — never core 1 */
    cfg.task_priority    = 5;
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 32;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.close_fn         = on_close;
    httpd_handle_t h = NULL;
    esp_err_t e = httpd_start(&h, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e)); return e; }
    s_hd = h;
    for (int i = 0; i < WS_MAX; i++) s_ws_fd[i] = -1;
    task_create_psram(ws_sender_task, "ws_sender", 4096, NULL, 6, NULL, 0);

    const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,     .handler = h_root },
        { .uri = "/index.html",       .method = HTTP_GET,     .handler = h_root },
        { .uri = "/api/v1/info",      .method = HTTP_GET,     .handler = h_info },
        { .uri = "/api/v1/status",    .method = HTTP_GET,     .handler = h_status },
        { .uri = "/api/v1/schema",    .method = HTTP_GET,     .handler = h_schema },
        { .uri = "/api/v1/config",    .method = HTTP_GET,     .handler = h_config_get },
        { .uri = "/api/v1/config",    .method = HTTP_PATCH,   .handler = h_config_patch },
        { .uri = "/api/v1/wifi",      .method = HTTP_GET,     .handler = h_wifi_get },
        { .uri = "/api/v1/wifi",      .method = HTTP_POST,    .handler = h_wifi_post },
        { .uri = "/api/v1/wifi/*",    .method = HTTP_DELETE,  .handler = h_wifi_delete },
        { .uri = "/api/v1/reboot",    .method = HTTP_POST,    .handler = h_reboot },
        { .uri = "/api/v1/power/off", .method = HTTP_POST,    .handler = h_power_off },
        { .uri = "/api/v1/selftest",  .method = HTTP_GET,     .handler = h_selftest },
        { .uri = "/api/v1/time",      .method = HTTP_POST,    .handler = h_time_post },
        { .uri = "/api/v1/recordings",        .method = HTTP_GET,    .handler = h_rec_list },
        { .uri = "/api/v1/recordings/start",  .method = HTTP_POST,   .handler = h_rec_start },
        { .uri = "/api/v1/recordings/stop",   .method = HTTP_POST,   .handler = h_rec_stop },
        { .uri = "/api/v1/recordings/synthetic", .method = HTTP_POST, .handler = h_rec_synth },
        { .uri = "/api/v1/recordings/*",      .method = HTTP_GET,    .handler = h_rec_data },     /* wildcard must end the template */
        { .uri = "/api/v1/recordings/*",      .method = HTTP_DELETE, .handler = h_rec_delete },
        { .uri = "/api/*",            .method = HTTP_OPTIONS, .handler = h_options },
        { .uri = "/ws",               .method = HTTP_GET,     .handler = h_ws, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(h, &uris[i]);
    ESP_LOGI(TAG, "HTTP server up on core 0 (%u B page)", (unsigned)index_html_gz_len);
    return ESP_OK;
}
