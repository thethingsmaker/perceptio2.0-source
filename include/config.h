/*
 * Perceptio 2.0 — persistent configuration (NVS namespace "cfg")
 *
 * Two kinds of data:
 *   1. The WiFi network list (blob "wifi_list").
 *   2. A registry of editable PARAMETERS. Each parameter is one NVS entry
 *      keyed by its name, and the registry describes it (type, range,
 *      options, unit, label) so the web UI and the mobile app render the
 *      settings form from /api/v1/schema instead of hard-coding it.
 *      Adding a parameter = one line in PARAM_TABLE in config.c.
 *
 * Rules (from the firmware brief): NVS is only written at boot or after a
 * session stops — never while the capture pipeline is running.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 21
#define FW_VERSION_STR   "0.3.21"

/* ------------------------------------------------------------------ */
/* WiFi list                                                           */
/* ------------------------------------------------------------------ */
#define CFG_WIFI_MAX      8
#define CFG_SSID_LEN      32
#define CFG_PASS_LEN      64

typedef struct {
    char ssid[CFG_SSID_LEN + 1];
    char pass[CFG_PASS_LEN + 1];   /* empty = open network */
} cfg_wifi_entry_t;

typedef struct {
    uint8_t           layout;      /* CFG_WIFI_LAYOUT */
    uint8_t           count;
    cfg_wifi_entry_t  list[CFG_WIFI_MAX];
} cfg_wifi_list_t;
#define CFG_WIFI_LAYOUT 1

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */
typedef enum { P_INT, P_BOOL, P_STR } cfg_ptype_t;

typedef struct {
    const char  *key;        /* NVS key and JSON name, <= 15 chars */
    const char  *label;      /* human label for the UI */
    const char  *group;      /* UI section */
    const char  *unit;       /* "" if none */
    cfg_ptype_t  type;
    void        *ptr;        /* int32_t*, bool*, or char[] */
    int32_t      min, max;   /* P_INT: range; P_STR: max length */
    const char  *options;    /* P_INT: comma list of allowed values ("" = any in range) */
    bool         reboot;     /* true = applied at next boot, false = applied live */
} cfg_param_t;

/* The live values. Read anywhere; write only through config_param_set_*. */
typedef struct {
    char    dev_name[24];    /* shown in UI, mDNS host name base */
    int32_t odr_hz;          /* IMU ODR, all channels: 480 / 960 / 1920 (HG ladder has no 800) */
    int32_t xl_fs_g;         /* low-g accel full scale: 2/4/8/16 */
    int32_t gy_fs_dps;       /* gyro full scale: 250..4000 */
    int32_t hg_fs_g;         /* high-g channel full scale: 32..320 */
    int32_t led_bright;      /* 0-100 % */
    bool    wifi_ps;         /* modem power save when idle */
    int32_t ws_hz;           /* live-stream flush rate to browsers */
    int32_t session_max_min; /* auto-stop a session after N minutes */
    bool    auto_upload;     /* upload sessions when WiFi is available */
    bool    selftest_boot;   /* run the quick self-test at every boot */
    bool    selftest_full;   /* + NAND stress and button wait (slow) */
    int32_t ichg_ma;         /* charge current while the device is ON (linear charger: heat!) */
    int32_t ilim_ma;         /* USB input current limit, shared by board + charger (500 starves the charger when ON) */
    int32_t chg_tmax_c;      /* pause charging when board (IMU die) temp exceeds this: ambient ~45 C + ~23 K self-heating */
    int32_t chg_tmin_c;      /* pause charging below this: ambient ~0 C + self-heating (Li-ion plating risk) */
    char    tz[48];          /* POSIX TZ string for local time display, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
    bool    log_serial;      /* log lines on the USB serial console (off for the ship build) */
    int32_t log_remote;      /* minimum level shipped to the backend: 0 none, 1 error, 2 warning, 3 info */
    char    ota_repo[80];    /* GitHub "owner/repo" whose latest Release carries the firmware .bin (ota.h) */
} cfg_values_t;

extern cfg_values_t g_cfg;

esp_err_t config_init(void);
const char *config_fw_version(void);
const char *config_prev_fw_version(void);   /* version stored in NVS by the previous boot when it differs ("" otherwise) */

/* registry access */
int                 config_param_count(void);
const cfg_param_t  *config_param(int i);
const cfg_param_t  *config_param_find(const char *key);

/* set from JSON-ish inputs; validates range/options; saves to NVS; fires the change hook */
esp_err_t config_param_set_int(const char *key, int32_t v);
esp_err_t config_param_set_bool(const char *key, bool v);
esp_err_t config_param_set_str(const char *key, const char *v);
/* Compact JSON of all parameter values (stable key order) — for config_hash. Returns length. */
int       config_dump_json(char *buf, size_t n);

/* Called after a parameter changed and was saved (only for reboot == false). */
typedef void (*cfg_change_fn)(const cfg_param_t *p);
void config_set_change_hook(cfg_change_fn fn);

/* WiFi list */
const cfg_wifi_list_t *config_wifi_get(void);
esp_err_t config_wifi_add(const char *ssid, const char *pass);
esp_err_t config_wifi_remove(const char *ssid);
esp_err_t config_wifi_prefer(const char *ssid);   /* move to the top of the list = tried first (call config_wifi_save) */
void      config_wifi_clear(void);
esp_err_t config_wifi_save(void);

esp_err_t config_factory_reset(void);

/* Capture-session lock: while true every NVS write (parameters, WiFi list) is refused with
 * ESP_ERR_INVALID_STATE — nothing touches the internal flash during a recording. */
void      config_set_locked(bool locked);
bool      config_is_locked(void);

/* Power state flag (NVS "user_off"): set when the user switches the device off, cleared when switched on.
 * Decides at boot whether a cable insertion means "charge quietly" or "run". */
bool      config_user_off_get(void);
esp_err_t config_user_off_set(bool off);
