#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "config.h"
#include "wifi_defaults.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CFG";
#define NS          "cfg"
#define KEY_FWVER   "fw_version"
#define KEY_WIFI    "wifi_list"

cfg_values_t g_cfg = {
    .dev_name        = "Perceptio",
    .odr_hz          = 960,
    .xl_fs_g         = 16,
    .gy_fs_dps       = 4000,
    .hg_fs_g         = 320,
    .led_bright      = 60,
    .wifi_ps         = false,
    .ws_hz           = 60,
    .session_max_min = 150,
    .auto_upload     = true,
    .selftest_boot   = true,
    .selftest_full   = false,
    .ichg_ma         = 300,
    .ilim_ma = 700,
    .chg_tmax_c      = 70,
    .chg_tmin_c      = 10,
    .tz              = "CET-1CEST,M3.5.0,M10.5.0/3",
    .log_serial = true,
    .log_remote = 3,
    .ota_repo   = "thethingsmaker/perceptio2.0-firmware",
};
static char s_prev_fw[16];

/* ---- the registry: one line per editable parameter ---------------- */
#define P_I(k, l, g, u, f, lo, hi, opt, rb) { k, l, g, u, P_INT,  &g_cfg.f, lo, hi, opt, rb }
#define P_B(k, l, g,    f,              rb) { k, l, g, "", P_BOOL, &g_cfg.f, 0, 1, "", rb }
#define P_S(k, l, g,    f, len,         rb) { k, l, g, "", P_STR,  &g_cfg.f, 0, len, "", rb }

static const cfg_param_t PARAM_TABLE[] = {
    P_S("dev_name",   "Device name",            "Device",  dev_name, sizeof g_cfg.dev_name - 1, true),
    P_I("odr_hz",     "IMU sample rate",        "Capture", "Hz",  odr_hz,    480, 1920, "480,960,1920",              true),
    P_I("xl_fs_g",    "Accelerometer range",    "Capture", "g",   xl_fs_g,   2,   16,   "2,4,8,16",                  true),
    P_I("gy_fs_dps",  "Gyroscope range",        "Capture", "dps", gy_fs_dps, 250, 4000, "250,500,1000,2000,4000",    true),
    P_I("hg_fs_g",    "High-g range",           "Capture", "g",   hg_fs_g,   32,  320,  "32,64,128,256,320",         true),
    P_I("session_max","Max session length",     "Capture", "min", session_max_min, 10, 600, "",                      false),
    P_B("auto_upload","Auto-upload sessions",   "Capture", auto_upload, false),
    P_I("led_bright", "LED brightness",         "Device",  "%",   led_bright, 0,  100,  "",                          false),
    P_B("wifi_ps",    "WiFi power save (idle)", "Network", wifi_ps, false),
    P_I("ws_hz",      "Live stream rate",       "Network", "fps", ws_hz,     10,  60,   "10,20,30,60",               false),
    P_B("selftest",   "Self-test at boot",      "Device",  selftest_boot, true),
    P_B("selftest_full","Full self-test (slow)","Device",  selftest_full, true),
    P_I("ichg_ma",    "Charge current",         "Power",   "mA",  ichg_ma,   100, 500,  "100,200,300,400,500",       false),
    P_I("ilim_ma",    "USB input limit",        "Power",   "mA",  ilim_ma,   100, 1100, "100,200,300,400,500,700,1100", false),
    P_I("chg_tmax_c", "Charge pause above",     "Power",   "°C",  chg_tmax_c, 50, 85,   "",                          false),
    P_I("chg_tmin_c", "Charge pause below",     "Power",   "°C",  chg_tmin_c, 0,  25,   "",                          false),
    P_S("tz",         "Time zone (POSIX)",      "Device",  tz, sizeof g_cfg.tz - 1, false),
    P_B("log_serial", "Serial console log",     "Debug", log_serial, false),
    P_I("log_remote", "Remote log level",       "Debug", "",  log_remote, 0, 3, "0,1,2,3",                           false),
    P_S("ota_repo",   "Firmware repository",    "Update", ota_repo, sizeof g_cfg.ota_repo - 1, false),
};
#define N_PARAMS (sizeof PARAM_TABLE / sizeof PARAM_TABLE[0])

static cfg_wifi_list_t s_wifi;
static cfg_change_fn   s_hook;

/* ------------------------------------------------------------------ */
static esp_err_t nvs_open_rw(nvs_handle_t *h) { return nvs_open(NS, NVS_READWRITE, h); }

static void load_param(nvs_handle_t h, const cfg_param_t *p)
{
    switch (p->type) {
    case P_INT:  { int32_t v; if (nvs_get_i32(h, p->key, &v) == ESP_OK) *(int32_t *)p->ptr = v; break; }
    case P_BOOL: { uint8_t v; if (nvs_get_u8(h, p->key, &v) == ESP_OK)  *(bool *)p->ptr = v != 0; break; }
    case P_STR: {
        char tmp[128];
        size_t n = p->max + 1;
        if (n > sizeof tmp) n = sizeof tmp;
        if (nvs_get_str(h, p->key, tmp, &n) == ESP_OK) {
            strncpy((char *)p->ptr, tmp, p->max);
            ((char *)p->ptr)[p->max] = 0;
        }
        break;
    }
    }
}

static volatile bool s_locked;
void config_set_locked(bool locked) { s_locked = locked; }
bool config_is_locked(void) { return s_locked; }

static esp_err_t save_param(const cfg_param_t *p)
{
    nvs_handle_t h; esp_err_t e = nvs_open_rw(&h);
    if (e != ESP_OK) return e;
    switch (p->type) {
    case P_INT:  e = nvs_set_i32(h, p->key, *(int32_t *)p->ptr); break;
    case P_BOOL: e = nvs_set_u8 (h, p->key, *(bool *)p->ptr);    break;
    case P_STR:  e = nvs_set_str(h, p->key, (char *)p->ptr);     break;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static void seed_wifi_defaults(void)
{
    static const cfg_wifi_entry_t defaults[] = WIFI_DEFAULTS;
    memset(&s_wifi, 0, sizeof s_wifi);
    s_wifi.layout = CFG_WIFI_LAYOUT;
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0] && i < CFG_WIFI_MAX; i++)
        s_wifi.list[s_wifi.count++] = defaults[i];
    ESP_LOGW(TAG, "no WiFi list in NVS — seeded %u default(s)", s_wifi.count);
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    if ((err = nvs_open_rw(&h)) != ESP_OK) return err;

    char stored[16] = {0}; size_t len = sizeof stored;
    if (nvs_get_str(h, KEY_FWVER, stored, &len) != ESP_OK || strcmp(stored, FW_VERSION_STR) != 0) {
        ESP_LOGI(TAG, "fw_version: '%s' -> '%s'", stored, FW_VERSION_STR);
        snprintf(s_prev_fw, sizeof s_prev_fw, "%s", stored);
        nvs_set_str(h, KEY_FWVER, FW_VERSION_STR);
    }

    for (size_t i = 0; i < N_PARAMS; i++) load_param(h, &PARAM_TABLE[i]);

    len = sizeof s_wifi;
    err = nvs_get_blob(h, KEY_WIFI, &s_wifi, &len);
    bool valid = err == ESP_OK && len == sizeof s_wifi && s_wifi.layout == CFG_WIFI_LAYOUT && s_wifi.count <= CFG_WIFI_MAX;
    if (!valid) { seed_wifi_defaults(); nvs_set_blob(h, KEY_WIFI, &s_wifi, sizeof s_wifi); }
    nvs_commit(h);
    nvs_close(h);
    for (int i = 0; i < CFG_WIFI_MAX; i++) { s_wifi.list[i].ssid[CFG_SSID_LEN] = 0; s_wifi.list[i].pass[CFG_PASS_LEN] = 0; }

    ESP_LOGI(TAG, "fw %s, dev '%s', odr %ld Hz, %u WiFi network(s)", FW_VERSION_STR, g_cfg.dev_name, (long)g_cfg.odr_hz, s_wifi.count);
    return ESP_OK;
}

const char *config_fw_version(void) { return FW_VERSION_STR; }
const char *config_prev_fw_version(void) { return s_prev_fw; }

/* ------------------------------------------------------------------ */
int config_param_count(void) { return N_PARAMS; }
const cfg_param_t *config_param(int i) { return (i >= 0 && i < (int)N_PARAMS) ? &PARAM_TABLE[i] : NULL; }
const cfg_param_t *config_param_find(const char *key)
{
    for (size_t i = 0; i < N_PARAMS; i++) if (strcmp(PARAM_TABLE[i].key, key) == 0) return &PARAM_TABLE[i];
    return NULL;
}
void config_set_change_hook(cfg_change_fn fn) { s_hook = fn; }

static bool in_options(const cfg_param_t *p, int32_t v)
{
    if (!p->options || !*p->options) return true;
    const char *s = p->options;
    while (*s) { if (strtol(s, (char **)&s, 10) == v) return true; if (*s == ',') s++; }
    return false;
}

static esp_err_t after_set(const cfg_param_t *p)
{
    esp_err_t e = save_param(p);
    ESP_LOGI(TAG, "set %s (%s)", p->key, esp_err_to_name(e));
    if (e == ESP_OK && !p->reboot && s_hook) s_hook(p);
    return e;
}

esp_err_t config_param_set_int(const char *key, int32_t v)
{
    const cfg_param_t *p = config_param_find(key);
    if (!p || p->type != P_INT) return ESP_ERR_NOT_FOUND;
    if (s_locked) return ESP_ERR_INVALID_STATE;
    if (v < p->min || v > p->max || !in_options(p, v)) return ESP_ERR_INVALID_ARG;
    *(int32_t *)p->ptr = v;
    return after_set(p);
}
esp_err_t config_param_set_bool(const char *key, bool v)
{
    const cfg_param_t *p = config_param_find(key);
    if (!p || p->type != P_BOOL) return ESP_ERR_NOT_FOUND;
    if (s_locked) return ESP_ERR_INVALID_STATE;
    *(bool *)p->ptr = v;
    return after_set(p);
}
esp_err_t config_param_set_str(const char *key, const char *v)
{
    const cfg_param_t *p = config_param_find(key);
    if (!p || p->type != P_STR) return ESP_ERR_NOT_FOUND;
    if (s_locked) return ESP_ERR_INVALID_STATE;
    if (!v || strlen(v) > (size_t)p->max) return ESP_ERR_INVALID_ARG;
    strcpy((char *)p->ptr, v);
    return after_set(p);
}

/* ------------------------------------------------------------------ */
const cfg_wifi_list_t *config_wifi_get(void) { return &s_wifi; }

static int find(const char *ssid)
{
    for (int i = 0; i < s_wifi.count; i++) if (strcmp(s_wifi.list[i].ssid, ssid) == 0) return i;
    return -1;
}
esp_err_t config_wifi_add(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid || strlen(ssid) > CFG_SSID_LEN) return ESP_ERR_INVALID_ARG;
    if (pass && strlen(pass) > CFG_PASS_LEN) return ESP_ERR_INVALID_ARG;
    int i = find(ssid);
    if (i < 0) { if (s_wifi.count >= CFG_WIFI_MAX) return ESP_ERR_NO_MEM; i = s_wifi.count++; }
    memset(&s_wifi.list[i], 0, sizeof s_wifi.list[i]);
    strncpy(s_wifi.list[i].ssid, ssid, CFG_SSID_LEN);
    if (pass) strncpy(s_wifi.list[i].pass, pass, CFG_PASS_LEN);
    return ESP_OK;
}
esp_err_t config_wifi_prefer(const char *ssid)
{
    int i = find(ssid);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    cfg_wifi_entry_t e = s_wifi.list[i];
    for (; i > 0; i--) s_wifi.list[i] = s_wifi.list[i - 1];
    s_wifi.list[0] = e;
    return ESP_OK;
}
esp_err_t config_wifi_remove(const char *ssid)
{
    int i = find(ssid);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    for (; i < s_wifi.count - 1; i++) s_wifi.list[i] = s_wifi.list[i + 1];
    memset(&s_wifi.list[--s_wifi.count], 0, sizeof s_wifi.list[0]);
    return ESP_OK;
}
void config_wifi_clear(void) { memset(&s_wifi, 0, sizeof s_wifi); s_wifi.layout = CFG_WIFI_LAYOUT; }
esp_err_t config_wifi_save(void)
{
    if (s_locked) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h; esp_err_t e = nvs_open_rw(&h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, KEY_WIFI, &s_wifi, sizeof s_wifi);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
bool config_user_off_get(void)
{
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, "user_off", &v);
    nvs_close(h);
    return v != 0;
}
esp_err_t config_user_off_set(bool off)
{
    nvs_handle_t h; esp_err_t e = nvs_open_rw(&h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "user_off", off);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h; esp_err_t e = nvs_open_rw(&h);
    if (e != ESP_OK) return e;
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int config_dump_json(char *buf, size_t n)
{
    size_t o = 0;
    o += snprintf(buf + o, n - o, "{");
    for (size_t i = 0; i < N_PARAMS && o < n; i++) {
        const cfg_param_t *p = &PARAM_TABLE[i];
        switch (p->type) {
        case P_INT:  o += snprintf(buf + o, n - o, "%s\"%s\":%ld", i ? "," : "", p->key, (long)*(int32_t *)p->ptr); break;
        case P_BOOL: o += snprintf(buf + o, n - o, "%s\"%s\":%s", i ? "," : "", p->key, *(bool *)p->ptr ? "true" : "false"); break;
        case P_STR:  o += snprintf(buf + o, n - o, "%s\"%s\":\"%s\"", i ? "," : "", p->key, (char *)p->ptr); break;
        }
    }
    o += snprintf(buf + o, n - o, "}");
    return (int)o;
}
