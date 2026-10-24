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
#include "cloud.h"
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
#define PAT_MAX       120                         /* classic ghp_ = 40, fine-grained github_pat_ = 93 */
#define JSON_MAX      (24 * 1024)                 /* releases/latest answer (PSRAM) */
#define MIN_HEAP      (24 * 1024)                 /* internal RAM needed, measured inside the job task (its 7 KB stack already taken):
                                                   * same floor as upload.c, where a TLS session runs at ~19 KB minimum */
#define UA            "Perceptio/" FW_VERSION_STR " (ota)"

static ota_status_t s_st;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static int s_op;                                  /* 1 check, 2 install */
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

static void do_check(void)
{
    char slug[80], url[160];
    if (!repo_slug(slug, sizeof slug)) { set_phase(OTA_ERROR, "ota_repo must be owner/repo"); return; }
    if (!heap_ok()) return;
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", slug);
    set_phase(OTA_CHECKING, NULL);
    ESP_LOGI(TAG, "checking %s", url);

    char *body = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { set_phase(OTA_ERROR, "no PSRAM for the answer"); return; }
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .buffer_size = 2048, .buffer_size_tx = 1024, .user_agent = UA,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); set_phase(OTA_ERROR, "http client init"); return; }
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");

    char err[64] = "";
    int n = 0, status = 0;
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
            if (rs == 200) snprintf(err, sizeof err, "repo OK, but no published release (drafts/pre-releases ignored)");
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

static void do_install(void)
{
    char asset[160]; snprintf(asset, sizeof asset, "%s", s_st.asset_url);
    if (!asset[0]) { set_phase(OTA_ERROR, "check first"); return; }
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
    ESP_LOGI(TAG, "downloading %s (%lu B) %s", s_st.asset_name, (unsigned long)s_st.asset_bytes, direct ? "directly" : "from the CDN");

    /* 2. stream it into the other slot */
    esp_http_client_config_t hc = {
        .url = direct ? asset : s_location, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 30000,
        .buffer_size = 4096, .buffer_size_tx = 2560, .user_agent = UA, .keep_alive_enable = true,   /* request line carries the ~1 KB signed URL */
    };
    esp_https_ota_config_t oc = { .http_config = &hc, .http_client_init_cb = direct ? ota_client_init_cb : NULL, .bulk_flash_erase = false };
    esp_https_ota_handle_t h = NULL;
    e = esp_https_ota_begin(&oc, &h);
    if (e != ESP_OK) { snprintf(err, sizeof err, "ota begin: %s", esp_err_to_name(e)); set_phase(OTA_ERROR, err); esp_wifi_set_ps(ps_prev); return; }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) == ESP_OK) ESP_LOGI(TAG, "image: %s %s (%s %s)", desc.project_name, desc.version, desc.date, desc.time);

    int64_t t0 = esp_timer_get_time(), last_log = t0;
    for (;;) {
        e = esp_https_ota_perform(h);
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        s_st.received_bytes = esp_https_ota_get_image_len_read(h);
        int64_t now = esp_timer_get_time();
        if (now - last_log > 5 * 1000000) {
            last_log = now;
            ESP_LOGI(TAG, "%lu / %lu B (%u KB/s)", (unsigned long)s_st.received_bytes, (unsigned long)s_st.asset_bytes,
                     (unsigned)(s_st.received_bytes / ((now - t0) / 1000 + 1)));
        }
    }
    s_st.received_bytes = esp_https_ota_get_image_len_read(h);
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
    set_phase(OTA_REBOOTING, NULL);
    char d[96]; snprintf(d, sizeof d, "{\"from\":\"%s\",\"to\":\"%s\",\"bytes\":%lu}", FW_VERSION_STR, s_st.latest_tag, (unsigned long)s_st.received_bytes);
    cloud_event("fw_update_installed", d);
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
    if (op == 1) do_check();
    else if (op == 2) do_install();
    free(s_location); s_location = NULL;
    s_task = NULL; s_op = 0;
    vTaskDelete(NULL);
}

static esp_err_t kick(int op)
{
    if (s_op || s_task || (s_st.phase != OTA_IDLE && s_st.phase != OTA_ERROR)) return ESP_ERR_INVALID_STATE;
    if (store_is_recording() || upload_active()) return ESP_ERR_INVALID_STATE;
    if (op == 2 && !s_st.checked) return ESP_ERR_NOT_FOUND;
    s_op = op;
    if (xTaskCreatePinnedToCore(ota_task, "ota", 7168, (void *)(intptr_t)op, 4, &s_task, 0) != pdPASS) {
        s_op = 0; s_task = NULL;
        set_phase(OTA_ERROR, "no memory for the OTA task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t ota_check(void)   { return kick(1); }
esp_err_t ota_install(void) { return kick(2); }
bool ota_busy(void) { return s_op || (s_st.phase != OTA_IDLE && s_st.phase != OTA_ERROR); }
void ota_get_status(ota_status_t *st) { portENTER_CRITICAL(&s_mux); *st = s_st; portEXIT_CRITICAL(&s_mux); }

bool ota_pending_verify(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}
void ota_mark_valid(void)
{
    if (!ota_pending_verify()) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "image %s marked valid (%s)", FW_VERSION_STR, esp_err_to_name(e));
}

esp_err_t ota_init(void)
{
    pat_load();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "running %s (%s), fw %s, repo '%s', token %s",
             run ? run->label : "?", st == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING VERIFY" : st == ESP_OTA_IMG_VALID ? "valid" : "undefined",
             FW_VERSION_STR, g_cfg.ota_repo, s_pat[0] ? "set" : "not set");
    return ESP_OK;                                /* the worker task is created per job (kick) */
}
