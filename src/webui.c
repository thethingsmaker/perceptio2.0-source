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
#include "ota.h"
#include "bus.h"
#include "app.h"
#include "net.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "task_psram.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "cJSON.h"
#include "store.h"
#include "cloud.h"
#include "esp_core_dump.h"
#include "esp_flash.h"

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

/* Browser title "Perceptio2.0-<xxx>-<yyy>": xxx = last octet of the current IP ("000" when offline),
 * yyy = reserved for future use (TITLE_YYY until it gets a meaning). */
#define TITLE_YYY "000"
static void board_title(const char *ip, char out[40])
{
    const char *dot = ip ? strrchr(ip, '.') : NULL;
    int oct = dot ? atoi(dot + 1) : 0;
    if (!ip || !*ip || strcmp(ip, "0.0.0.0") == 0) oct = 0;
    snprintf(out, 40, "Perceptio2.0-%03d-%s", oct, TITLE_YYY);
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
    { char t[40]; board_title(st.ip, t); cJSON_AddStringToObject(j, "title", t); }
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
    if (st.soc_pct >= 0) cJSON_AddNumberToObject(j, "soc_pct", st.soc_pct); else cJSON_AddNullToObject(j, "soc_pct");
    cJSON_AddBoolToObject  (j, "battery", st.soc_pct >= 0);           /* false = no cell connected (USB only) */
    cJSON_AddBoolToObject  (j, "vin", st.bq_stat0 & 1);
    cJSON_AddNumberToObject(j, "chg", (st.bq_stat0 >> 5) & 3);
    cJSON_AddNumberToObject(j, "rssi", st.rssi);
    cJSON_AddStringToObject(j, "ip", st.ip);
    { char t[40]; board_title(st.ip, t); cJSON_AddStringToObject(j, "title", t); }
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
    { ota_status_t os; ota_get_status(&os);
      cJSON *o = cJSON_AddObjectToObject(j, "ota");
      static const char *ph[] = { "idle", "checking", "downloading", "verifying", "rebooting", "error" };
      cJSON_AddStringToObject(o, "phase", ph[os.phase <= OTA_ERROR ? os.phase : 0]);
      if (os.phase == OTA_DOWNLOADING) { cJSON_AddNumberToObject(o, "received_bytes", os.received_bytes); cJSON_AddNumberToObject(o, "total_bytes", os.asset_bytes); }
      if (os.phase == OTA_ERROR) cJSON_AddStringToObject(o, "error", os.error); }
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
        cJSON_AddBoolToObject(o, "connected", net_is_connected() && strcmp(net_ssid(), l->list[i].ssid) == 0);
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

static esp_err_t h_wifi_scan(httpd_req_t *r)            /* GET /api/v1/wifi/scan — blocks ~2-3 s */
{
    net_ap_t *ap = heap_caps_malloc(24 * sizeof *ap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ap) return send_err(r, "500 Internal Server Error", "no memory");
    int n = net_scan_list(ap, 24);
    if (n < 0) { free(ap); return send_err(r, "409 Conflict", "WiFi busy (connecting) — try again in a few seconds"); }
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ap[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", ap[i].rssi);
        cJSON_AddNumberToObject(o, "channel", ap[i].channel);
        cJSON_AddBoolToObject(o, "open", ap[i].open);
        cJSON_AddItemToArray(a, o);
    }
    free(ap);
    return send_json(r, a, NULL);
}

static cJSON *wifi_test_json(void)
{
    net_test_t t; net_test_get(&t);
    static const char *st[] = { "idle", "running", "ok", "failed" };
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", st[t.state <= NT_FAIL ? t.state : 0]);
    cJSON_AddStringToObject(j, "ssid", t.ssid);
    if (t.state == NT_RUNNING) cJSON_AddStringToObject(j, "step", t.step);
    if (t.state == NT_OK) {
        cJSON_AddStringToObject(j, "ip", t.ip);
        cJSON_AddNumberToObject(j, "rssi", t.rssi);
        cJSON_AddBoolToObject(j, "internet", t.internet);
        cJSON_AddBoolToObject(j, "saved", t.saved);
    }
    if (t.state == NT_FAIL) { cJSON_AddStringToObject(j, "error", t.error); cJSON_AddNumberToObject(j, "reason", t.reason); }
    cJSON_AddStringToObject(j, "mode", t.stay ? "connect" : "test");
    cJSON_AddStringToObject(j, "current_ssid", net_is_connected() ? net_ssid() : "");
    return j;
}
static esp_err_t h_wifi_test_get(httpd_req_t *r) { return send_json(r, wifi_test_json(), NULL); }
/* POST /api/v1/wifi/test {ssid, pass}: join it, check DHCP + DNS, save on success, go back (see net.h). */
static esp_err_t h_wifi_test_post(httpd_req_t *r)
{
    if (config_is_locked() || store_is_recording()) return send_err(r, "409 Conflict", "recording in progress");
    if (upload_active()) return send_err(r, "409 Conflict", "upload in progress");
    if (ota_busy()) return send_err(r, "409 Conflict", "firmware update in progress");
    cJSON *b = read_body(r);
    const cJSON *ssid = b ? cJSON_GetObjectItem(b, "ssid") : NULL;
    const cJSON *pass = b ? cJSON_GetObjectItem(b, "pass") : NULL;
    esp_err_t e = cJSON_IsString(ssid) ? net_test_start(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(b);
    if (e == ESP_ERR_INVALID_ARG) return send_err(r, "400 Bad Request", "SSID 1-32 chars; password empty (open) or 8-64 chars");
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "a test is already running");
    if (e != ESP_OK) return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    return send_json(r, wifi_test_json(), "202 Accepted");
}

/* POST /api/v1/wifi/connect {ssid}: switch to a saved network now (stays there if it works; see net.h). */
static esp_err_t h_wifi_connect(httpd_req_t *r)
{
    if (store_is_recording()) return send_err(r, "409 Conflict", "recording in progress");
    if (upload_active()) return send_err(r, "409 Conflict", "upload in progress");
    if (ota_busy()) return send_err(r, "409 Conflict", "firmware update in progress");
    cJSON *b = read_body(r);
    const cJSON *ssid = b ? cJSON_GetObjectItem(b, "ssid") : NULL;
    esp_err_t e = cJSON_IsString(ssid) ? net_switch_start(ssid->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(b);
    if (e == ESP_ERR_NOT_FOUND) return send_err(r, "404 Not Found", "not a saved network");
    if (e == ESP_ERR_INVALID_SIZE) return send_err(r, "409 Conflict", "already connected to it");
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", "a test or connection is already running");
    if (e != ESP_OK) return send_err(r, "400 Bad Request", esp_err_to_name(e));
    return send_json(r, wifi_test_json(), "202 Accepted");
}
/* POST /api/v1/wifi/prefer {ssid}: move it to the top of the list (tried first at boot and on retries). */
static esp_err_t h_wifi_prefer(httpd_req_t *r)
{
    if (config_is_locked()) return send_err(r, "409 Conflict", "recording in progress: settings are locked");
    cJSON *b = read_body(r);
    const cJSON *ssid = b ? cJSON_GetObjectItem(b, "ssid") : NULL;
    esp_err_t e = cJSON_IsString(ssid) ? config_wifi_prefer(ssid->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(b);
    if (e == ESP_OK) e = config_wifi_save();
    if (e == ESP_ERR_NOT_FOUND) return send_err(r, "404 Not Found", "not a saved network");
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
/* Browsers ask for /favicon.ico on their own; answer "no content" (cached for a day) instead of a logged 404. */
static esp_err_t h_favicon(httpd_req_t *r)
{
    httpd_resp_set_status(r, "204 No Content");
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=86400");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t h_reboot(httpd_req_t *r)
{
    cors(r);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    app_reboot("ui");                             /* does not return */
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
    app_power_off("ui");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* /api/v1/ota — firmware update from GitHub Releases (ota.h)           */
/* ------------------------------------------------------------------ */
static cJSON *ota_json(void)
{
    ota_status_t os; ota_get_status(&os);
    cJSON *j = cJSON_CreateObject();
    static const char *ph[] = { "idle", "checking", "downloading", "verifying", "rebooting", "error" };
    cJSON_AddStringToObject(j, "phase", ph[os.phase <= OTA_ERROR ? os.phase : 0]);
    cJSON_AddStringToObject(j, "running", config_fw_version());
    { const esp_partition_t *p = esp_ota_get_running_partition(); cJSON_AddStringToObject(j, "slot", p ? p->label : "?"); }
    cJSON_AddBoolToObject(j, "pending_verify", ota_pending_verify());
    cJSON_AddStringToObject(j, "repo", g_cfg.ota_repo);
    { char hint[16]; ota_pat_hint(hint); cJSON_AddBoolToObject(j, "token_set", ota_pat_is_set()); cJSON_AddStringToObject(j, "token_hint", hint); }
    cJSON_AddBoolToObject(j, "checked", os.checked);
    if (os.checked) {
        cJSON_AddStringToObject(j, "latest", os.latest_tag);
        cJSON_AddStringToObject(j, "asset", os.asset_name);
        cJSON_AddNumberToObject(j, "asset_bytes", os.asset_bytes);
        cJSON_AddStringToObject(j, "published", os.published);
        cJSON_AddBoolToObject(j, "update_available", os.update_available);
    }
    if (os.phase == OTA_DOWNLOADING) { cJSON_AddNumberToObject(j, "received_bytes", os.received_bytes); cJSON_AddNumberToObject(j, "total_bytes", os.asset_bytes); }
    if (os.phase == OTA_ERROR) cJSON_AddStringToObject(j, "error", os.error);
    return j;
}
static esp_err_t h_ota_get(httpd_req_t *r) { return send_json(r, ota_json(), NULL); }

/* Crash evidence: the core dump image as a file (decode: esp-coredump info_corefile -t raw -c <file> <firmware.elf>).
 * Reading/erasing the internal flash stalls both cores, so both are refused while recording. */
static esp_err_t h_coredump_get(httpd_req_t *r)
{
    if (store_is_recording()) return send_err(r, "409 Conflict", "recording in progress");
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0)
        return send_err(r, "404 Not Found", "no core dump in flash");
    char cd[96]; snprintf(cd, sizeof cd, "attachment; filename=\"coredump-%s-%s.bin\"", cloud_device_id(), config_fw_version());
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    static uint8_t buf[1024];                         /* internal (.bss): one request at a time (httpd is single-threaded) */
    for (size_t off = 0; off < size; off += sizeof buf) {
        size_t n = size - off < sizeof buf ? size - off : sizeof buf;
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK) { httpd_resp_send_chunk(r, NULL, 0); return ESP_FAIL; }
        if (httpd_resp_send_chunk(r, (const char *)buf, n) != ESP_OK) return ESP_FAIL;
    }
    ESP_LOGI(TAG, "core dump downloaded (%u B)", (unsigned)size);
    return httpd_resp_send_chunk(r, NULL, 0);
}
static esp_err_t h_coredump_delete(httpd_req_t *r)
{
    if (store_is_recording()) return send_err(r, "409 Conflict", "recording in progress");
    esp_err_t e = esp_core_dump_image_erase();
    ESP_LOGW(TAG, "core dump erased via API (%s)", esp_err_to_name(e));
    if (e != ESP_OK) return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "erased", true);
    return send_json(r, j, NULL);
}
static esp_err_t ota_kick(httpd_req_t *r, esp_err_t e)
{
    if (e == ESP_ERR_INVALID_STATE) return send_err(r, "409 Conflict", store_is_recording() ? "recording in progress" : upload_active() ? "upload in progress" : "update already running");
    if (e == ESP_ERR_NOT_FOUND) return send_err(r, "409 Conflict", "check for updates first");
    if (e == ESP_ERR_NOT_ALLOWED) return send_err(r, "409 Conflict", "battery below 20 %: plug the cable in to update");
    if (e != ESP_OK) return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    vTaskDelay(pdMS_TO_TICKS(50));
    return send_json(r, ota_json(), NULL);
}
static esp_err_t h_ota_check(httpd_req_t *r)   { return ota_kick(r, ota_check()); }
static esp_err_t h_ota_install(httpd_req_t *r) { return ota_kick(r, ota_install()); }
/* POST {"token":"ghp_…"} ("" clears). The token is never returned. */
static esp_err_t h_ota_token(httpd_req_t *r)
{
    if (config_is_locked()) return send_err(r, "409 Conflict", "recording in progress: settings are locked");
    cJSON *b = read_body(r);
    cJSON *t = b ? cJSON_GetObjectItem(b, "token") : NULL;
    if (!cJSON_IsString(t)) { cJSON_Delete(b); return send_err(r, "400 Bad Request", "{\"token\":\"…\"} expected"); }
    esp_err_t e = ota_pat_set(t->valuestring);
    cJSON_Delete(b);
    if (e == ESP_ERR_INVALID_ARG) return send_err(r, "400 Bad Request", "token: letters, digits and _ only, max 120");
    if (e != ESP_OK) return send_err(r, "500 Internal Server Error", esp_err_to_name(e));
    return send_json(r, ota_json(), NULL);
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
    app_err_t e = app_rec_start(tagbuf, "ui", rid);
    if (e == APP_BUSY)         return send_err(r, "409 Conflict", "already recording");
    if (e == APP_STORAGE_FULL) return send_err(r, "507 Insufficient Storage", "storage full");
    if (e == APP_BATTERY_LOW)  return send_err(r, "409 Conflict", "battery low (< 5 %): connect the charger");
    if (e == APP_BAD_ARGS)     return send_err(r, "400 Bad Request", "rec_tag: max 64 characters");
    if (e != APP_OK)           return send_err(r, "500 Internal Server Error", app_reason(e));
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
    app_err_t e = app_rec_generate(minutes, tagbuf, "ui");
    if (e == APP_BUSY)         return send_err(r, "409 Conflict", "already recording");
    if (e == APP_STORAGE_FULL) return send_err(r, "507 Insufficient Storage", "storage full");
    if (e != APP_OK)           return send_err(r, "500 Internal Server Error", app_reason(e));
    cJSON *j = cJSON_CreateObject(); cJSON_AddNumberToObject(j, "minutes", minutes);
    return send_json(r, j, NULL);
}

static esp_err_t h_rec_stop(httpd_req_t *r)
{
    store_rec_t done;
    app_err_t e = app_rec_stop("ui", NULL, &done);
    if (e != APP_OK) return send_err(r, "409 Conflict", "not recording");
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
    app_err_t e = app_rec_delete(rid, "ui");
    if (e == APP_UNKNOWN_RECORDING || e == APP_BAD_ARGS) return send_err(r, "404 Not Found", "unknown recording");
    if (e == APP_BUSY)              return send_err(r, "409 Conflict", "recording in progress");
    if (e != APP_OK)                return send_err(r, "500 Internal Server Error", app_reason(e));
    bus_inventory_changed();
    return h_rec_list(r);
}

/* ------------------------------------------------------------------ */
/* live stream: core-1 producer -> ring -> core-0 WebSocket sender     */
/* ------------------------------------------------------------------ */
#define RING_N   4096
#define WS_MAX   4
/* 4096 x 20 B = 80 KB: in PSRAM (allocated by webui_start). It was a static array until 0.3.24, i.e. 80 KB of the
 * internal RAM the WiFi/TLS buffers need. PSRAM is safe here: the only writer is the sampler TASK (paused, like every
 * task, while the flash cache is off) and the only reader is ws_sender (a task). Samples before webui_start are dropped. */
static webui_sample_t *volatile s_ring;
static volatile uint32_t s_wr = 0;
static int s_ws_fd[WS_MAX];
static volatile int s_stream_ms = 16;

void webui_feed(const imu_sample_t *smp, int64_t t_us)
{
    (void)t_us;
    webui_sample_t *ring = s_ring;
    if (!ring) return;
    uint32_t w = s_wr;
    ring[w % RING_N] = *smp;
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
        webui_sample_t *ring = s_ring;
        if (!any || !ring) { rd = wr; continue; }
        if (wr - rd > RING_N) rd = wr - RING_N;
        while (rd != wr) {
            uint32_t n = wr - rd; if (n > 64) n = 64;
            memcpy(frame, &rd, 4);
            for (uint32_t k = 0; k < n; k++) memcpy(frame + 4 + k * sizeof(webui_sample_t), &ring[(rd + k) % RING_N], sizeof(webui_sample_t));
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
    if (s_hd) return ESP_OK;                      /* already running (e.g. the fallback AP came up later) */
    s_status_fn = fn;
    if (!s_ring) {
        webui_sample_t *ring = heap_caps_calloc(RING_N, sizeof *ring, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring) ESP_LOGE(TAG, "no PSRAM for the live-stream ring (%u B): Live tab disabled", (unsigned)(RING_N * sizeof *ring));
        s_ring = ring;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id          = 0;                      /* UI core — never core 1 */
    cfg.task_priority    = 5;
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 40;                    /* 34 registered; headroom */
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
        { .uri = "/favicon.ico",      .method = HTTP_GET,     .handler = h_favicon },
        { .uri = "/api/v1/info",      .method = HTTP_GET,     .handler = h_info },
        { .uri = "/api/v1/status",    .method = HTTP_GET,     .handler = h_status },
        { .uri = "/api/v1/schema",    .method = HTTP_GET,     .handler = h_schema },
        { .uri = "/api/v1/config",    .method = HTTP_GET,     .handler = h_config_get },
        { .uri = "/api/v1/config",    .method = HTTP_PATCH,   .handler = h_config_patch },
        { .uri = "/api/v1/wifi",      .method = HTTP_GET,     .handler = h_wifi_get },
        { .uri = "/api/v1/wifi",      .method = HTTP_POST,    .handler = h_wifi_post },
        { .uri = "/api/v1/wifi/scan", .method = HTTP_GET,     .handler = h_wifi_scan },
        { .uri = "/api/v1/wifi/test", .method = HTTP_GET,     .handler = h_wifi_test_get },
        { .uri = "/api/v1/wifi/test", .method = HTTP_POST,    .handler = h_wifi_test_post },
        { .uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = h_wifi_connect },
        { .uri = "/api/v1/wifi/prefer",  .method = HTTP_POST, .handler = h_wifi_prefer },
        { .uri = "/api/v1/wifi/*",    .method = HTTP_DELETE,  .handler = h_wifi_delete },
        { .uri = "/api/v1/reboot",    .method = HTTP_POST,    .handler = h_reboot },
        { .uri = "/api/v1/power/off", .method = HTTP_POST,    .handler = h_power_off },
        { .uri = "/api/v1/ota",         .method = HTTP_GET,   .handler = h_ota_get },
        { .uri = "/api/v1/coredump",    .method = HTTP_GET,    .handler = h_coredump_get },
        { .uri = "/api/v1/coredump",    .method = HTTP_DELETE, .handler = h_coredump_delete },
        { .uri = "/api/v1/ota/check",   .method = HTTP_POST,  .handler = h_ota_check },
        { .uri = "/api/v1/ota/install", .method = HTTP_POST,  .handler = h_ota_install },
        { .uri = "/api/v1/ota/token",   .method = HTTP_POST,  .handler = h_ota_token },
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
