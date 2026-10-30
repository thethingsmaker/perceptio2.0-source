/* Firmware update from GitHub Releases — see include/ota.h. */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include "ota.h"
#include "config.h"
#include "store.h"
#include "upload.h"
#include "bus.h"
#include "power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "OTA";

#define NS_CFG        "cfg"                       /* same namespace as config.c */
#define KEY_PAT       "ota_pat"
#define KEY_TGT       "ota_tgt"                   /* "from>to" while an installed image has not proved itself (0.3.37) */
#define PAT_MAX       120                         /* classic ghp_ = 40, fine-grained github_pat_ = 93 */
#define JSON_MAX      (24 * 1024)                 /* releases/latest answer (PSRAM) */
#define MIN_HEAP      (24 * 1024)                 /* internal RAM needed, measured inside the job task (its 7 KB stack already taken):
                                                   * same floor as upload.c, where a TLS session runs at ~19 KB minimum */
#define UA            "Perceptio/" FW_VERSION_STR " (ota)"

static ota_status_t s_st;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static int s_op;                                  /* 1 check, 2 install, 3 remote update */
static struct { char id[24]; char tag[24]; bool force; ota_answer_fn answer; } s_rq;   /* op 3 request */
static char s_pat[PAT_MAX + 1];
#define LOC_MAX 2048
static char *s_location;                          /* redirect target (signed CDN URL, ~1 KB): PSRAM, only during an install */

static void set_phase(ota_phase_t p, const char *err)
{
    portENTER_CRITICAL(&s_mux);
    s_st.phase = p;
    if (err) snprintf(s_st.error, sizeof s_st.error, "%s", err); else if (p != OTA_ERROR) s_st.error[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    if (p == OTA_ERROR) ESP_LOGE(TAG, "%s", err ? err : "error");
}

/* ------------------------------------------------------------------ */
/* PAT in NVS                                                          */
/* ------------------------------------------------------------------ */
static void pat_load(void)
{
    nvs_handle_t h; s_pat[0] = 0;
    if (nvs_open(NS_CFG, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_pat;
    if (nvs_get_str(h, KEY_PAT, s_pat, &len) != ESP_OK) s_pat[0] = 0;
    nvs_close(h);
}
esp_err_t ota_pat_set(const char *pat)
{
    if (config_is_locked()) return ESP_ERR_INVALID_STATE;
    if (!pat) pat = "";
    if (strlen(pat) > PAT_MAX) return ESP_ERR_INVALID_ARG;
    for (const char *c = pat; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_') return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = *pat ? nvs_set_str(h, KEY_PAT, pat) : nvs_erase_key(h, KEY_PAT);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) { strcpy(s_pat, pat); ESP_LOGI(TAG, "token %s", *pat ? "stored" : "cleared"); }
    return e;
}
bool ota_pat_is_set(void) { return s_pat[0] != 0; }
void ota_pat_hint(char out[16])
{
    size_t n = strlen(s_pat);
    if (n < 12) { out[0] = 0; return; }
    snprintf(out, 16, "%.4s…%s", s_pat, s_pat + n - 4);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
/* "https://github.com/owner/repo.git/" -> "owner/repo" */
static bool repo_slug(char *out, size_t n)
{
    const char *r = g_cfg.ota_repo;
    const char *p = strstr(r, "github.com/"); if (p) r = p + 11;
    while (*r == '/') r++;
    size_t len = strlen(r);
    while (len && r[len - 1] == '/') len--;
    if (len > 4 && strncmp(r + len - 4, ".git", 4) == 0) len -= 4;
    if (len == 0 || len >= n) return false;
    int slashes = 0;
    for (size_t i = 0; i < len; i++) {
        char c = r[i];
        if (c == '/') slashes++;
        else if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    if (slashes != 1) return false;
    memcpy(out, r, len); out[len] = 0;
    return true;
}

/* Compare "v1.2.3" / "1.2.3" numerically; 0 equal, <0 a older. Unparseable -> strcmp. */
static int ver_cmp(const char *a, const char *b)
{
    int av[3] = {0}, bv[3] = {0};
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    if (sscanf(a, "%d.%d.%d", &av[0], &av[1], &av[2]) < 2 || sscanf(b, "%d.%d.%d", &bv[0], &bv[1], &bv[2]) < 2) return strcmp(a, b);
    for (int i = 0; i < 3; i++) if (av[i] != bv[i]) return av[i] - bv[i];
    return 0;
}

static void add_auth(esp_http_client_handle_t c)
{
    if (!s_pat[0]) return;
    char h[PAT_MAX + 8];
    snprintf(h, sizeof h, "Bearer %s", s_pat);
    esp_http_client_set_header(c, "Authorization", h);
}

static esp_err_t on_http(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && strcasecmp(ev->header_key, "Location") == 0)
        if (s_location) snprintf(s_location, LOC_MAX, "%s", ev->header_value);
    return ESP_OK;
}

static bool heap_ok(void)
{
    size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (heap >= MIN_HEAP) return true;
    char m[64]; snprintf(m, sizeof m, "low memory: %u KB internal free", (unsigned)(heap >> 10));
    set_phase(OTA_ERROR, m);
    return false;
}
static unsigned heap_kb(void) { return (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10); }
/* OTA_MIN_SOC_PCT (ota.h): on battery below the floor -> refuse; cable in or no battery (soc -1) -> allowed */
static bool battery_too_low(void)
{
    bq25180_status_t bq;
    bool plugged = power_charger_status(&bq) == ESP_OK && bq.vin_pgood;
    int pct = power_soc_display();
    return !plugged && pct >= 0 && pct < OTA_MIN_SOC_PCT;
}

/* "error" event {code:"ota_failed", msg, version} (protocol: Remote firmware update) */
static void ota_failed_event(const char *msg, const char *version)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "code", "ota_failed");
    cJSON_AddStringToObject(j, "msg", msg ? msg : "");
    cJSON_AddStringToObject(j, "version", version ? version : "");
    char *t = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (t) { bus_event("error", t); free(t); }
}

#define OTA_STACK     7168
static unsigned stack_peak(void) { return OTA_STACK - (unsigned)uxTaskGetStackHighWaterMark(NULL); }   /* bytes (IDF counts bytes) */

/* ------------------------------------------------------------------ */
/* check                                                               */
/* ------------------------------------------------------------------ */
/* GET https://api.github.com/repos/<slug>: 200 = the token sees the repo. Used to explain a 404 on releases/latest. */
static int repo_status(const char *slug)
{
    char url[160]; snprintf(url, sizeof url, "https://api.github.com/repos/%s", slug);
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .buffer_size = 2048, .buffer_size_tx = 1024, .user_agent = UA,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) { esp_http_client_fetch_headers(c); status = esp_http_client_get_status_code(c); }
    esp_http_client_close(c); esp_http_client_cleanup(c);
    return status;
}

/* tag NULL = releases/latest, else releases/tags/<tag>. Returns 0 found, 404 = that tag has no release (repo
 * visible), -1 any other error (reason in s_st.error, phase OTA_ERROR). */
static int do_check(const char *tag_wanted)
{
    char slug[80], url[160];
    if (!repo_slug(slug, sizeof slug)) { set_phase(OTA_ERROR, "ota_repo must be owner/repo"); return -1; }
    if (!heap_ok()) return -1;
    if (tag_wanted) snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/tags/%s", slug, tag_wanted);
    else            snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", slug);
    set_phase(OTA_CHECKING, NULL);
    ESP_LOGI(TAG, "checking %s", url);

    char *body = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { set_phase(OTA_ERROR, "no PSRAM for the answer"); return -1; }
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .buffer_size = 2048, .buffer_size_tx = 1024, .user_agent = UA,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); set_phase(OTA_ERROR, "http client init"); return -1; }
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");

    char err[64] = "";
    int n = 0, status = 0, rc = 0;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) snprintf(err, sizeof err, "connect: %s", esp_err_to_name(e));
    else {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        for (;;) {
            int r = esp_http_client_read(c, body + n, JSON_MAX - 1 - n);
            if (r <= 0) break;
            n += r;
            if (n >= JSON_MAX - 1) break;
        }
        body[n] = 0;
    }
    esp_http_client_close(c); esp_http_client_cleanup(c);

    if (!err[0]) {
        if (status == 401) snprintf(err, sizeof err, "GitHub 401: token missing or wrong");
        else if (status == 404) {                 /* GitHub answers 404 both for "no release" and for "no access": tell them apart */
            int rs = repo_status(slug);
            if (rs == 200 && tag_wanted) { snprintf(err, sizeof err, "no release tagged %s", tag_wanted); rc = 404; }
            else if (rs == 200) snprintf(err, sizeof err, "repo OK, but no published release (drafts/pre-releases ignored)");
            else if (rs == 404) snprintf(err, sizeof err, s_pat[0] ? "repo not visible with this token" : "repo not found (private? set a token)");
            else snprintf(err, sizeof err, "GitHub 404 on releases; repo check HTTP %d", rs);
        }
        else if (status == 403) snprintf(err, sizeof err, "GitHub 403: token lacks access or rate limited");
        else if (status != 200) snprintf(err, sizeof err, "GitHub HTTP %d", status);
    }
    if (!err[0]) {
        cJSON *j = cJSON_ParseWithLength(body, n);
        cJSON *tag = j ? cJSON_GetObjectItem(j, "tag_name") : NULL;
        cJSON *assets = j ? cJSON_GetObjectItem(j, "assets") : NULL;
        cJSON *pub = j ? cJSON_GetObjectItem(j, "published_at") : NULL;
        /* The app image: "firmware.bin" if present, else the first .bin that is not the bootloader, partition table
         * or OTA data (a release may carry all of PlatformIO's build outputs). */
        cJSON *pick = NULL;
        cJSON *a; cJSON_ArrayForEach(a, assets) {
            cJSON *nm = cJSON_GetObjectItem(a, "name");
            if (!cJSON_IsString(nm)) continue;
            const char *v = nm->valuestring; size_t l = strlen(v);
            if (l <= 4 || strcmp(v + l - 4, ".bin") != 0) continue;
            if (strcmp(v, "firmware.bin") == 0) { pick = a; break; }
            if (strstr(v, "bootloader") || strstr(v, "partition") || strstr(v, "ota_data")) continue;
            if (!pick) pick = a;
        }
        if (!cJSON_IsString(tag)) snprintf(err, sizeof err, "%s", n >= JSON_MAX - 1 ? "release JSON too large" : "release JSON: no tag_name");
        else if (!pick) snprintf(err, sizeof err, "release %s has no .bin asset", tag->valuestring);
        else {
            cJSON *au = cJSON_GetObjectItem(pick, "url"), *an = cJSON_GetObjectItem(pick, "name"), *as = cJSON_GetObjectItem(pick, "size");
            portENTER_CRITICAL(&s_mux);
            snprintf(s_st.latest_tag, sizeof s_st.latest_tag, "%s", tag->valuestring);
            snprintf(s_st.asset_name, sizeof s_st.asset_name, "%s", cJSON_IsString(an) ? an->valuestring : "");
            snprintf(s_st.asset_url, sizeof s_st.asset_url, "%s", cJSON_IsString(au) ? au->valuestring : "");
            snprintf(s_st.published, sizeof s_st.published, "%s", cJSON_IsString(pub) ? pub->valuestring : "");
            s_st.asset_bytes = cJSON_IsNumber(as) ? (uint32_t)as->valuedouble : 0;
            s_st.checked = true;
            s_st.update_available = ver_cmp(s_st.latest_tag, FW_VERSION_STR) != 0;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "latest %s (%s, %lu B, %s) — running %s%s", s_st.latest_tag, s_st.asset_name, (unsigned long)s_st.asset_bytes,
                     s_st.published, FW_VERSION_STR, s_st.update_available ? ": update available" : ": up to date");
        }
        cJSON_Delete(j);
    }
    free(body);
    set_phase(err[0] ? OTA_ERROR : OTA_IDLE, err[0] ? err : NULL);
    return err[0] ? (rc ? rc : -1) : 0;
}

/* ------------------------------------------------------------------ */
/* install                                                             */
/* ------------------------------------------------------------------ */
static esp_err_t ota_client_init_cb(esp_http_client_handle_t c)
{
    /* Only used when the asset URL answered 200 directly (no CDN redirect): then it needs our headers. */
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/octet-stream");
    return ESP_OK;
}

static ota_mode_fn s_enter, s_leave;
void ota_set_update_mode(ota_mode_fn enter, ota_mode_fn leave) { s_enter = enter; s_leave = leave; }

static void install(const char *asset);              /* returns only on failure: an installed image reboots */
static void do_install(void)
{
    char asset[160]; snprintf(asset, sizeof asset, "%s", s_st.asset_url);
    if (!asset[0]) { set_phase(OTA_ERROR, "check first"); return; }
    /* "downloading" (0 %) BEFORE entering update mode: the UI polls only while a job reports a busy phase, and the
     * install request is answered ~50 ms after the job starts — entering update mode takes 1-2 s (0.3.21 bug). */
    s_st.received_bytes = 0;
    set_phase(OTA_DOWNLOADING, NULL);
    bus_state("updating");                            /* published before update mode switches the link off */
    ESP_LOGI(TAG, "install: %u KB internal free, entering update mode", heap_kb());
    if (s_enter) s_enter();
    ESP_LOGI(TAG, "install: %u KB internal free in update mode", heap_kb());
    install(asset);
    ESP_LOGW(TAG, "install failed: leaving update mode");
    bus_state("idle");                                /* kick() refuses during a recording/upload: idle is right */
    ota_failed_event(s_st.error, s_st.latest_tag);    /* queued; replayed after the next hello */
    if (s_leave) s_leave();
}

static void install(const char *asset)
{
    if (!heap_ok()) return;
    set_phase(OTA_DOWNLOADING, NULL);
    s_st.received_bytes = 0;
    wifi_ps_type_t ps_prev = WIFI_PS_NONE; esp_wifi_get_ps(&ps_prev);
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 1. resolve the asset to its CDN URL without following the redirect (the CDN rejects a request that carries
     *    both the Authorization header and the signed query string). */
    if (!s_location) s_location = heap_caps_malloc(LOC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_location) { set_phase(OTA_ERROR, "no PSRAM for the redirect URL"); esp_wifi_set_ps(ps_prev); return; }
    s_location[0] = 0;
    int status = 0; esp_err_t e;
    {
        esp_http_client_config_t cfg = {
            .url = asset, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000, .event_handler = on_http,
            .buffer_size = 4096, .buffer_size_tx = 1024, .user_agent = UA, .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) { set_phase(OTA_ERROR, "http client init"); esp_wifi_set_ps(ps_prev); return; }
        add_auth(c);
        esp_http_client_set_header(c, "Accept", "application/octet-stream");
        e = esp_http_client_open(c, 0);
        if (e == ESP_OK) { esp_http_client_fetch_headers(c); status = esp_http_client_get_status_code(c); }
        esp_http_client_close(c); esp_http_client_cleanup(c);
    }
    char err[64] = "";
    if (e != ESP_OK) snprintf(err, sizeof err, "asset connect: %s", esp_err_to_name(e));
    else if ((status == 302 || status == 301 || status == 307) && !s_location[0]) snprintf(err, sizeof err, "redirect without Location");
    else if (status != 302 && status != 301 && status != 307 && status != 200) snprintf(err, sizeof err, "asset HTTP %d", status);
    if (err[0]) { set_phase(OTA_ERROR, err); esp_wifi_set_ps(ps_prev); return; }
    bool direct = status == 200;
    if (!direct) {
        size_t ll = strlen(s_location);
        const char *host = strstr(s_location, "://"); host = host ? host + 3 : s_location;
        ESP_LOGI(TAG, "redirect to %.*s (URL %u chars)%s", (int)strcspn(host, "/?"), host, (unsigned)ll, ll >= LOC_MAX - 1 ? " TRUNCATED" : "");
        if (ll >= LOC_MAX - 1) { set_phase(OTA_ERROR, "redirect URL too long"); esp_wifi_set_ps(ps_prev); return; }
    }
    ESP_LOGI(TAG, "downloading %s (%lu B) %s — %u KB internal free", s_st.asset_name, (unsigned long)s_st.asset_bytes,
             direct ? "directly" : "from the CDN", heap_kb());

    /* 2. stream it into the other slot */
    esp_http_client_config_t hc = {
        .url = direct ? asset : s_location, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 30000,
        .buffer_size = 4096, .buffer_size_tx = 2560, .user_agent = UA, .keep_alive_enable = true,   /* request line carries the ~1 KB signed URL */
    };
    esp_https_ota_config_t oc = { .http_config = &hc, .http_client_init_cb = direct ? ota_client_init_cb : NULL, .bulk_flash_erase = false };
    esp_https_ota_handle_t h = NULL;
    e = esp_https_ota_begin(&oc, &h);
    if (e != ESP_OK) { snprintf(err, sizeof err, "ota begin: %s", esp_err_to_name(e)); set_phase(OTA_ERROR, err); esp_wifi_set_ps(ps_prev); return; }
    ESP_LOGI(TAG, "session open: %u KB internal free", heap_kb());

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) == ESP_OK) ESP_LOGI(TAG, "image: %s %s (%s %s)", desc.project_name, desc.version, desc.date, desc.time);

    int64_t t0 = esp_timer_get_time(), last_log = t0;
    size_t hmin = SIZE_MAX;                           /* lowest internal free heap seen during the download */
    for (;;) {
        int64_t c0 = esp_timer_get_time();
        e = esp_https_ota_perform(h);
        int64_t call_ms = (esp_timer_get_time() - c0) / 1000;
        if (call_ms > 2000)                           /* diagnostic: the ~30 s stall after the first ~64 KB (2026-10-29) */
            ESP_LOGW(TAG, "slow read: %lld ms, now at %d B (%s), %u KB internal free", call_ms,
                     esp_https_ota_get_image_len_read(h), esp_err_to_name(e), heap_kb());
        size_t hf = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); if (hf < hmin) hmin = hf;
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        s_st.received_bytes = esp_https_ota_get_image_len_read(h);
        int64_t now = esp_timer_get_time();
        if (now - last_log > 5 * 1000000) {
            last_log = now;
            ESP_LOGI(TAG, "%lu / %lu B (%u KB/s), heap %u KB (min %u KB)", (unsigned long)s_st.received_bytes, (unsigned long)s_st.asset_bytes,
                     (unsigned)(s_st.received_bytes / ((now - t0) / 1000 + 1)), (unsigned)(hf >> 10), (unsigned)(hmin >> 10));
        }
    }
    s_st.received_bytes = esp_https_ota_get_image_len_read(h);
    ESP_LOGI(TAG, "download ended (%s): %lu B in %lld s, heap min %u KB, stack peak %u of %u B", esp_err_to_name(e),
             (unsigned long)s_st.received_bytes, (esp_timer_get_time() - t0) / 1000000, (unsigned)(hmin >> 10), stack_peak(), OTA_STACK);
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) { e = ESP_FAIL; snprintf(err, sizeof err, "incomplete download"); }
    else if (e != ESP_OK) snprintf(err, sizeof err, "download: %s", esp_err_to_name(e));
    if (e != ESP_OK) { esp_https_ota_abort(h); set_phase(OTA_ERROR, err); esp_wifi_set_ps(ps_prev); return; }

    set_phase(OTA_VERIFYING, NULL);
    e = esp_https_ota_finish(h);                      /* validates the image and sets the boot partition */
    if (e != ESP_OK) {
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) snprintf(err, sizeof err, "image validation failed");
        else snprintf(err, sizeof err, "ota finish: %s", esp_err_to_name(e));
        set_phase(OTA_ERROR, err); esp_wifi_set_ps(ps_prev); return;
    }
    ESP_LOGW(TAG, "installed %s (%lu B in %lld s): rebooting", s_st.latest_tag, (unsigned long)s_st.received_bytes, (esp_timer_get_time() - t0) / 1000000);
    {   /* boot outcome breadcrumb (ota.h): the next boot tells success from rollback */
        const char *to = s_st.latest_tag[0] == 'v' ? s_st.latest_tag + 1 : s_st.latest_tag;
        char tgt[64]; snprintf(tgt, sizeof tgt, "%s>%.31s", FW_VERSION_STR, to);
        nvs_handle_t nh;
        if (nvs_open(NS_CFG, NVS_READWRITE, &nh) == ESP_OK) { nvs_set_str(nh, KEY_TGT, tgt); nvs_commit(nh); nvs_close(nh); }
    }
    set_phase(OTA_REBOOTING, NULL);
    char d[96]; snprintf(d, sizeof d, "{\"from\":\"%s\",\"to\":\"%s\",\"bytes\":%lu}", FW_VERSION_STR, s_st.latest_tag, (unsigned long)s_st.received_bytes);
    bus_event("fw_update_installed", d);
    vTaskDelay(pdMS_TO_TICKS(2500));                  /* let the event and the UI answer go out */
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* One task per job, deleted when done: its stack must be internal (esp_https_ota writes the flash with the cache
 * disabled, which a PSRAM stack cannot survive), so it is only allocated while a check/install runs. */
static void ota_task(void *arg)
{
    int op = (int)(intptr_t)arg;
    s_st.last_check_ms = esp_timer_get_time() / 1000;
    if (op == 1) {
        /* the check opens a TLS session to api.github.com too: same update mode as an install (0.3.24 — on 0.3.23 a
         * check found 19 KB and refused). Busy phase first so the UI keeps polling; a check always leaves again. */
        set_phase(OTA_CHECKING, NULL);
        ESP_LOGI(TAG, "check: %u KB internal free, entering update mode", heap_kb());
        if (s_enter) s_enter();
        ESP_LOGI(TAG, "check: %u KB internal free in update mode", heap_kb());
        do_check(NULL);
        if (s_leave) s_leave();
    }
    else if (op == 2) do_install();
    else if (op == 3) {
        /* remote update: the lookup runs with the cloud link UP, so the answer (applied / updated:false / bad_args)
         * goes out on it; update mode only starts for the install itself (~80 KB internal free with MQTT up, 0.3.36). */
        set_phase(OTA_CHECKING, NULL);
        ESP_LOGI(TAG, "remote update (cmd %s): %s%s, force %s, %u KB internal free", s_rq.id, s_rq.tag[0] ? "release " : "latest release",
                 s_rq.tag, s_rq.force ? "yes" : "no", heap_kb());
        int rc = do_check(s_rq.tag[0] ? s_rq.tag : NULL);
        ota_status_t st; ota_get_status(&st);
        if (rc == 404)     s_rq.answer(s_rq.id, OTA_R_BAD_VERSION, FW_VERSION_STR, s_rq.tag, 0, st.error);
        else if (rc != 0)  s_rq.answer(s_rq.id, OTA_R_ERROR, FW_VERSION_STR, s_rq.tag, 0, st.error);
        else if (!s_rq.force && ver_cmp(st.latest_tag, FW_VERSION_STR) <= 0) {
            ESP_LOGI(TAG, "remote update: %s is not newer than %s, nothing to do", st.latest_tag, FW_VERSION_STR);
            s_rq.answer(s_rq.id, OTA_R_NOT_NEWER, FW_VERSION_STR, st.latest_tag, 0, NULL);
        } else {
            const char *to = st.latest_tag[0] == 'v' ? st.latest_tag + 1 : st.latest_tag;
            ESP_LOGW(TAG, "remote update: %s -> %s (%lu B)", FW_VERSION_STR, to, (unsigned long)st.asset_bytes);
            bus_state("updating");                    /* before the applied ack: same order as the protocol */
            s_rq.answer(s_rq.id, OTA_R_UPDATING, FW_VERSION_STR, to, st.asset_bytes, NULL);
            do_install();                             /* returns only on failure (event ota_failed sent by it) */
        }
    }
    ESP_LOGI(TAG, "%s job done: stack peak %u of %u B, %u KB internal free", op == 1 ? "check" : op == 2 ? "install" : "remote update", stack_peak(), OTA_STACK, heap_kb());
    free(s_location); s_location = NULL;
    s_task = NULL; s_op = 0;
    vTaskDelete(NULL);
}

static esp_err_t kick(int op)
{
    if (s_op || s_task || (s_st.phase != OTA_IDLE && s_st.phase != OTA_ERROR)) return ESP_ERR_INVALID_STATE;
    if (store_is_recording() || upload_active()) return ESP_ERR_INVALID_STATE;
    if (op == 2 && !s_st.checked) return ESP_ERR_NOT_FOUND;
    if (op != 1 && battery_too_low()) {
        ESP_LOGW(TAG, "%s refused: battery %d %% < %d %% and no cable", op == 2 ? "install" : "remote update", power_soc_display(), OTA_MIN_SOC_PCT);
        return ESP_ERR_NOT_ALLOWED;
    }
    s_op = op;
    if (xTaskCreatePinnedToCore(ota_task, "ota", OTA_STACK, (void *)(intptr_t)op, 4, &s_task, 0) != pdPASS) {
        s_op = 0; s_task = NULL;
        set_phase(OTA_ERROR, "no memory for the OTA task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t ota_check(void)   { return kick(1); }
esp_err_t ota_install(void) { return kick(2); }

esp_err_t ota_remote_update(const char *cmd_id, const char *version, bool force, ota_answer_fn answer)
{
    if (!cmd_id || !answer) return ESP_ERR_INVALID_ARG;
    char tag[24] = "";
    if (version && *version) {                        /* "0.3.36" / "v0.3.36" -> "v0.3.36" (our release tags) */
        const char *v = (*version == 'v' || *version == 'V') ? version + 1 : version;
        size_t l = strlen(v);
        if (l == 0 || l > 16) return ESP_ERR_INVALID_ARG;
        for (const char *c = v; *c; c++) if (!isdigit((unsigned char)*c) && *c != '.') return ESP_ERR_INVALID_ARG;
        snprintf(tag, sizeof tag, "v%s", v);
    }
    if (s_op || s_task || (s_st.phase != OTA_IDLE && s_st.phase != OTA_ERROR)) return ESP_ERR_INVALID_STATE;
    snprintf(s_rq.id, sizeof s_rq.id, "%s", cmd_id);
    snprintf(s_rq.tag, sizeof s_rq.tag, "%s", tag);
    s_rq.force = force; s_rq.answer = answer;
    return kick(3);
}
bool ota_busy(void) { return s_op || (s_st.phase != OTA_IDLE && s_st.phase != OTA_ERROR); }
void ota_get_status(ota_status_t *st) { portENTER_CRITICAL(&s_mux); *st = s_st; portEXIT_CRITICAL(&s_mux); }

/* PENDING_VERIFY is read from otadata ONCE, in ota_init(), and kept in RAM. esp_ota_get_state_partition() maps the
 * otadata partition (esp_partition_mmap/munmap), which stops the caches of BOTH cores; called 10x/s from the main
 * loop it caused interrupt-watchdog resets during recordings (2026-10-28, boots 37/38/40, 0.3.19 and earlier). */
static bool s_pending;
static bool s_tgt_keep;                               /* the ota_tgt breadcrumb names this image: erase once it is valid */
bool ota_pending_verify(void) { return s_pending; }
void ota_mark_valid(void)
{
    if (!s_pending) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK) s_pending = false;
    ESP_LOGW(TAG, "image %s marked valid (%s)", FW_VERSION_STR, esp_err_to_name(e));
    if (e == ESP_OK && s_tgt_keep) {                  /* this image proved itself: the breadcrumb has served */
        nvs_handle_t h; if (nvs_open(NS_CFG, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, KEY_TGT); nvs_commit(h); nvs_close(h); }
        s_tgt_keep = false;
    }
}

/* boot: read the "from>to" breadcrumb left by the last install (ota.h) */
static void tgt_boot_check(void)
{
    nvs_handle_t h; char tgt[64]; size_t len = sizeof tgt;
    if (nvs_open(NS_CFG, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, KEY_TGT, tgt, &len) != ESP_OK) { nvs_close(h); return; }
    char *gt = strchr(tgt, '>');
    const char *from = tgt, *to = gt ? gt + 1 : "";
    if (gt) *gt = 0;
    bool erase = true;
    if (*to && ver_cmp(to, FW_VERSION_STR) == 0) {
        if (s_pending) { s_tgt_keep = true; erase = false; }   /* erased by ota_mark_valid once this image proved itself */
    } else if (*from && ver_cmp(from, FW_VERSION_STR) == 0) {
        char m[64]; snprintf(m, sizeof m, "%s did not boot: rolled back to %s", to, FW_VERSION_STR);
        ESP_LOGE(TAG, "last update failed: %s", m);
        ota_failed_event(m, to);                      /* buffered by the cloud until the first hello */
    }
    if (erase) { nvs_erase_key(h, KEY_TGT); nvs_commit(h); }
    nvs_close(h);
}

esp_err_t ota_init(void)
{
    pat_load();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);  /* the only otadata read of this boot */
    s_pending = st == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "running %s (%s), fw %s, repo '%s', token %s",
             run ? run->label : "?", st == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING VERIFY" : st == ESP_OTA_IMG_VALID ? "valid" : "undefined",
             FW_VERSION_STR, g_cfg.ota_repo, s_pat[0] ? "set" : "not set");
    tgt_boot_check();
    return ESP_OK;                                /* the worker task is created per job (kick) */
}
