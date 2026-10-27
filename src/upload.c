#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include "upload.h"
#include "store.h"
#include "bus.h"
#include "config.h"
#include "net.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "task_psram.h"
#include "cJSON.h"

static const char *TAG = "UPLOAD";

#define BODY_MAX        768
#define MIN_CHUNK       (64 * 1024)
#define MAX_CHUNK       (1024 * 1024)
#define GIVE_UP_MS      (3600 * 1000)           /* token lives 6 h; stop trying after 1 h of failures */
#define PROGRESS_MS     5000                    /* hello re-publish cadence while uploading */
#define MIN_HEAP        (24 * 1024)             /* internal RAM required before opening the HTTPS session (TLS buffers are in PSRAM;
                                                   the HW-AES bounce buffer per 4 KB record and the sockets are what stays internal) */
#define MAX_DROPS       8                       /* kept for stats.drops[] */

typedef struct { char cmd_id[40], rec_id[32], url[160], token[72]; uint32_t chunk; } job_t;
typedef struct { uint32_t offset; char step[12]; char err[28]; int errno_; int8_t rssi; } drop_t;

static upload_done_fn s_done;
static char s_devid[16];                              /* for the User-Agent; given by main at init */
static TaskHandle_t   s_task, s_reader;
static job_t          s_job;
static volatile bool  s_busy;
static upload_status_t s_st;
static char           s_body[BODY_MAX]; static int s_body_len;
static struct {
    uint32_t chunks, connects, retries, ndrops; int64_t read_us, send_us, t0; uint64_t bytes;
    int8_t rssi; char phy[12]; drop_t drops[MAX_DROPS]; size_t heap_min;
} s_stats;

/* prefetch: a reader task fills the other buffer while the sender transmits (NAND read overlaps the network) */
static SemaphoreHandle_t s_pf_req, s_pf_done;
static struct { uint32_t off, len; int got; uint8_t *buf; int64_t us; } s_pf;

/* ------------------------------------------------------------------ */
static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_body_len < BODY_MAX - 1) {
        int n = e->data_len; if (n > BODY_MAX - 1 - s_body_len) n = BODY_MAX - 1 - s_body_len;
        memcpy(s_body + s_body_len, e->data, n); s_body_len += n; s_body[s_body_len] = 0;
    }
    return ESP_OK;
}

static int8_t link_rssi(void) { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }
static void link_info(int8_t *rssi, char *phy, size_t n)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { *rssi = 0; snprintf(phy, n, "?"); return; }
    *rssi = ap.rssi;
    wifi_bandwidth_t bw = WIFI_BW_HT20; esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    snprintf(phy, n, "%s%s%s%s", ap.phy_11b ? "b" : "", ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "", bw == WIFI_BW_HT40 ? "/HT40" : "/HT20");
}

/* one request on a persistent client; returns the HTTP status or <0 on transport error; body in s_body */
static int request(esp_http_client_handle_t c, const char *url, esp_http_client_method_t m, const uint8_t *data, int len,
                   const char *crc_hdr, const char *ctype, const char *step, uint32_t offset)
{
    s_body_len = 0; s_body[0] = 0;
    esp_http_client_set_url(c, url);
    esp_http_client_set_method(c, m);
    esp_http_client_set_header(c, "X-Chunk-CRC32", crc_hdr);           /* NULL deletes the header */
    esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_post_field(c, (const char *)data, len);
    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        int en = esp_http_client_get_errno(c);
        snprintf(s_st.last_error, sizeof s_st.last_error, "%s errno %d", esp_err_to_name(e), en);
        drop_t d = { .offset = offset, .errno_ = en, .rssi = link_rssi() };
        strncpy(d.step, step, sizeof d.step - 1); strncpy(d.err, esp_err_to_name(e), sizeof d.err - 1);
        if (s_stats.ndrops < MAX_DROPS) s_stats.drops[s_stats.ndrops] = d;
        s_stats.ndrops++;
        ESP_LOGW(TAG, "drop at %" PRIu32 " during %s: %s (errno %d), rssi %d, heap %u KB", offset, step, d.err, en, d.rssi,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
        char ev[160];
        snprintf(ev, sizeof ev, "{\"rec_id\":\"%s\",\"offset\":%" PRIu32 ",\"step\":\"%s\",\"err\":\"%s\",\"errno\":%d,\"rssi\":%d}",
                 s_job.rec_id, offset, step, d.err, en, d.rssi);
        bus_event("upload_retry", ev);
        return -1;
    }
    return esp_http_client_get_status_code(c);
}

static int64_t body_num(const char *key)                 /* -1 when absent */
{
    cJSON *j = cJSON_Parse(s_body); if (!j) return -1;
    const cJSON *v = cJSON_GetObjectItem(j, key);
    int64_t r = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : -1;
    cJSON_Delete(j); return r;
}
static bool body_status_is(const char *s)
{
    cJSON *j = cJSON_Parse(s_body); if (!j) return false;
    const cJSON *v = cJSON_GetObjectItem(j, "status");
    bool r = cJSON_IsString(v) && strcmp(v->valuestring, s) == 0;
    cJSON_Delete(j); return r;
}

static void progress(uint32_t sent, bool force)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    s_st.sent_bytes = sent;
    if (force || now - last > PROGRESS_MS * 1000LL) { last = now; store_set_uploaded(s_job.rec_id, sent); bus_inventory_changed(); }
}

/* ------------------------------------------------------------------ */
static void reader_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_pf_req, portMAX_DELAY);
        int64_t t = esp_timer_get_time();
        s_pf.got = store_read(s_job.rec_id, s_pf.off, s_pf.buf, s_pf.len);
        s_pf.us = esp_timer_get_time() - t;
        xSemaphoreGive(s_pf_done);
    }
}
static void prefetch(uint8_t *buf, uint32_t off, uint32_t len) { s_pf.buf = buf; s_pf.off = off; s_pf.len = len; xSemaphoreGive(s_pf_req); }
static int  prefetch_wait(void) { xSemaphoreTake(s_pf_done, portMAX_DELAY); s_stats.read_us += s_pf.us; return s_pf.got; }

static void run(void)
{
    const store_rec_t *r = store_find(s_job.rec_id);
    if (!r) { s_done(s_job.cmd_id, false, "unknown_recording", NULL, NULL); return; }
    uint32_t total = r->bytes;
    memset(&s_st, 0, sizeof s_st);
    strncpy(s_st.rec_id, s_job.rec_id, sizeof s_st.rec_id - 1);
    s_st.total_bytes = total;

    uint8_t sha[32];
    if (store_sha256(s_job.rec_id, sha) != ESP_OK) { s_done(s_job.cmd_id, false, store_is_recording() ? "busy" : "io_error", NULL, NULL); return; }
    char shahex[65]; for (int i = 0; i < 32; i++) snprintf(shahex + 2 * i, 3, "%02x", sha[i]);

    uint8_t *bufs[2] = { heap_caps_malloc(s_job.chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), heap_caps_malloc(s_job.chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) };
    if (!bufs[0] || !bufs[1]) { free(bufs[0]); free(bufs[1]); s_done(s_job.cmd_id, false, "io_error", NULL, NULL); return; }

    char ua[64], bearer[88], url[200];
    snprintf(ua, sizeof ua, "Perceptio/%s (%s)", config_fw_version(), s_devid);
    snprintf(bearer, sizeof bearer, "Bearer %s", s_job.token);
    esp_http_client_config_t cfg = {
        .url = s_job.url, .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = on_http,
        .timeout_ms = 30000, .buffer_size = 2048, .buffer_size_tx = 4096, .user_agent = ua, .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = NULL;

    wifi_ps_type_t ps_prev = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps_prev);
    esp_wifi_set_ps(WIFI_PS_NONE);                        /* modem sleep delays TCP ACKs to the next beacon: off while uploading */
    memset(&s_stats, 0, sizeof s_stats); s_stats.t0 = esp_timer_get_time(); s_stats.heap_min = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    link_info(&s_stats.rssi, s_stats.phy, sizeof s_stats.phy);
    bus_state("uploading");
    ESP_LOGI(TAG, "%s: %" PRIu32 " bytes to %s (chunk %" PRIu32 ") — rssi %d dBm, phy %s, ps %d->0, heap %u KB, TLS record %d B",
             s_job.rec_id, total, s_job.url, s_job.chunk, s_stats.rssi, s_stats.phy, (int)ps_prev,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10), CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    int64_t fail_since = 0;
    uint32_t backoff_ms = 500; int sha_retries = 0;
    const char *fail = NULL; char result[BODY_MAX]; result[0] = 0;
    int cur = 0, got = 0; bool pf_pending = false, have = false;

    for (;;) {
        size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (heap < MIN_HEAP) {                           /* a TLS session needs ~30 KB; starting one with less starves lwIP */
            ESP_LOGW(TAG, "only %u KB internal heap free: waiting for %u KB", (unsigned)(heap >> 10), MIN_HEAP >> 10);
            snprintf(s_st.last_error, sizeof s_st.last_error, "low heap %u KB", (unsigned)(heap >> 10));
            goto net_error;
        }
        if (!c) { c = esp_http_client_init(&cfg); esp_http_client_set_header(c, "Authorization", bearer); s_stats.connects++; }

        /* 1. where to resume */
        s_st.phase = UP_RESUMING;
        int st = request(c, s_job.url, HTTP_METHOD_GET, NULL, 0, NULL, NULL, "get", 0);
        int64_t off = st == 200 ? body_num("received_bytes") : -1;
        if (off < 0) goto net_error;
        ESP_LOGI(TAG, "resume at %lld", (long long)off);
        progress((uint32_t)off, true);

        /* 2. chunks, double-buffered: the reader fills bufs[1-cur] while we send bufs[cur] */
        s_st.phase = UP_SENDING;
        if (pf_pending) { prefetch_wait(); pf_pending = false; }
        have = false;
        if ((uint32_t)off < total) { uint32_t w = total - (uint32_t)off; if (w > s_job.chunk) w = s_job.chunk; prefetch(bufs[cur], (uint32_t)off, w); pf_pending = true; }
        while ((uint32_t)off < total) {
            if (!have) {
                got = prefetch_wait(); pf_pending = false;
                if (got == -ESP_ERR_INVALID_STATE) { fail = "busy"; goto out; }
                if (got <= 0) { fail = "io_error"; goto out; }
                have = true;
                uint32_t nxt = (uint32_t)off + got;
                if (nxt < total) {                       /* read the next chunk while this one is on the wire */
                    uint32_t n = total - nxt; if (n > s_job.chunk) n = s_job.chunk;
                    prefetch(bufs[1 - cur], nxt, n); pf_pending = true;
                }
            }
            uint8_t *buf = bufs[cur];
            uint32_t next_off = (uint32_t)off + got;
            int64_t t_chunk = esp_timer_get_time();
            char crc[12]; snprintf(crc, sizeof crc, "%08" PRIx32, esp_rom_crc32_le(0, buf, got));
            snprintf(url, sizeof url, "%s?offset=%lld", s_job.url, (long long)off);
            st = request(c, url, HTTP_METHOD_PUT, buf, got, crc, "application/octet-stream", "put", (uint32_t)off);
            if (st == 200 || st == 409) {
                int64_t rb = body_num("received_bytes");
                if (rb < 0) goto net_error;
                int64_t dt = esp_timer_get_time() - t_chunk;
                s_stats.chunks++; s_stats.bytes += got; s_stats.send_us += dt;
                { size_t h = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); if (h < s_stats.heap_min) s_stats.heap_min = h; }
                ESP_LOGI(TAG, "chunk %lld+%d -> %lld: %lld ms (%d KB/s), %lld%%", (long long)off, got, (long long)rb,
                         (long long)(dt / 1000), dt > 0 ? (int)((int64_t)got * 1000 / dt) : 0, (long long)(rb * 100 / total));
                have = false;
                if (rb == next_off) { off = rb; cur = 1 - cur; }   /* the prefetched chunk is the right one */
                else {                                             /* server is elsewhere: re-read from its offset */
                    off = rb;
                    if (pf_pending) { prefetch_wait(); pf_pending = false; }
                    if ((uint32_t)off < total) { uint32_t n = total - (uint32_t)off; if (n > s_job.chunk) n = s_job.chunk; prefetch(bufs[cur], (uint32_t)off, n); pf_pending = true; }
                }
                progress((uint32_t)off, false);
                fail_since = 0; backoff_ms = 500;
            } else if (st == 422 || (st >= 500 && st < 600)) {
                ESP_LOGW(TAG, "chunk at %lld: HTTP %d, resending", (long long)off, st);   /* same buffer again */
                s_st.retries++; s_stats.retries++;
                vTaskDelay(pdMS_TO_TICKS(500));
            } else if (st == 401 || st == 403 || st == 404) {
                ESP_LOGE(TAG, "HTTP %d: %s", st, s_body); fail = "network"; goto out;
            } else goto net_error;
        }

        /* 3. finish */
        s_st.phase = UP_VERIFYING;
        char fin[96]; snprintf(fin, sizeof fin, "{\"size\":%" PRIu32 ",\"sha256\":\"%s\"}", total, shahex);
        snprintf(url, sizeof url, "%s/complete", s_job.url);
        st = request(c, url, HTTP_METHOD_POST, (const uint8_t *)fin, strlen(fin), NULL, "application/json", "complete", total);
        if (st == 200 && (body_status_is("verified") || body_status_is("verified_with_errors"))) {
            strncpy(result, s_body, sizeof result - 1);
            progress(total, true);
            ESP_LOGI(TAG, "%s: %s", s_job.rec_id, s_body);
            goto out;
        }
        if (st == 422 && body_status_is("sha_mismatch")) {
            ESP_LOGE(TAG, "server: sha mismatch — restarting from 0 (%d)", sha_retries);
            if (++sha_retries > 2) { fail = "io_error"; goto out; }
            continue;                                    /* step 1 again: the server reset the upload */
        }
        if (st == 401 || st == 403 || st == 404) { ESP_LOGE(TAG, "complete: HTTP %d %s", st, s_body); fail = "network"; goto out; }

net_error:
        if (c) { esp_http_client_cleanup(c); c = NULL; }    /* a failed connection leaves fragments: start clean next time */
        if (!fail_since) fail_since = esp_timer_get_time();
        if (esp_timer_get_time() - fail_since > GIVE_UP_MS * 1000LL) { fail = "network"; goto out; }
        s_st.phase = UP_BACKOFF; s_st.retries++; s_stats.retries++;
        bool wifi_up = net_is_connected();
        if (!wifi_up && backoff_ms < 2000) backoff_ms = 2000;      /* link down: 2, 4, 8 .. 60 s; link up: 0.5, 1, 2 .. 60 s */
        ESP_LOGW(TAG, "network error (%s, wifi %s): retry in %" PRIu32 " ms", s_st.last_error, wifi_up ? "up" : "down", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > 60000) backoff_ms = 60000;
        if (store_is_recording()) { fail = "busy"; goto out; }
    }
out:
    if (pf_pending) prefetch_wait();
    if (c) esp_http_client_cleanup(c);
    free(bufs[0]); free(bufs[1]);
    esp_wifi_set_ps(ps_prev);
    s_st.phase = UP_IDLE;
    if (!store_is_recording()) bus_state("idle");

    /* stats for the ack (firmware-upload-speed.md §1 + follow-up) */
    static char stats[640];
    int64_t total_ms = (esp_timer_get_time() - s_stats.t0) / 1000;
    int n = snprintf(stats, sizeof stats,
        "{\"chunks\":%" PRIu32 ",\"bytes\":%llu,\"total_ms\":%lld,\"read_ms\":%lld,\"send_ms\":%lld,\"connects\":%" PRIu32 ",\"retries\":%" PRIu32
        ",\"rssi\":%d,\"phy\":\"%s\",\"heap_min_kb\":%u,\"tcp_snd_buf\":%d,\"tls_out\":%d,\"drops\":[",
        s_stats.chunks, (unsigned long long)s_stats.bytes, (long long)total_ms, (long long)(s_stats.read_us / 1000), (long long)(s_stats.send_us / 1000),
        s_stats.connects, s_stats.retries, s_stats.rssi, s_stats.phy, (unsigned)(s_stats.heap_min >> 10),
        CONFIG_LWIP_TCP_SND_BUF_DEFAULT, CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    for (uint32_t i = 0; i < s_stats.ndrops && i < MAX_DROPS && n < (int)sizeof stats - 80; i++)
        n += snprintf(stats + n, sizeof stats - n, "%s{\"offset\":%" PRIu32 ",\"step\":\"%s\",\"err\":\"%s\",\"errno\":%d,\"rssi\":%d}",
                      i ? "," : "", s_stats.drops[i].offset, s_stats.drops[i].step, s_stats.drops[i].err, s_stats.drops[i].errno_, s_stats.drops[i].rssi);
    snprintf(stats + n, sizeof stats - n, "]}");
    ESP_LOGI(TAG, "%s: %s — ps restored to %d, stats %s", s_job.rec_id, fail ? fail : "done", (int)ps_prev, stats);
    if (fail) s_done(s_job.cmd_id, false, fail, NULL, stats);
    else s_done(s_job.cmd_id, true, NULL, result, stats);
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run();
        s_busy = false;
    }
}

/* ------------------------------------------------------------------ */
esp_err_t upload_init(upload_done_fn done, const char *device_id)
{
    strlcpy(s_devid, device_id ? device_id : "?", sizeof s_devid);
    s_done = done;
    /* the effective build values that decide upload throughput — compare with docs/firmware-upload-speed.md §2 */
#if defined(CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM)
    const char *txb = "dynamic"; int txn = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM;
#elif defined(CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM)
    const char *txb = "static"; int txn = CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM;   /* IDF default with PSRAM enabled */
#else
    const char *txb = "?"; int txn = 0;
#endif
    ESP_LOGI(TAG, "build: TCP_SND_BUF %d, TCP_WND %d, TCP_RECVMBOX %d, TCPIP_RECVMBOX %d, WIFI TX buffers %s x%d, WIFI/LWIP in PSRAM %s, TLS alloc %s, TLS out %d",
             CONFIG_LWIP_TCP_SND_BUF_DEFAULT, CONFIG_LWIP_TCP_WND_DEFAULT, CONFIG_LWIP_TCP_RECVMBOX_SIZE, CONFIG_LWIP_TCPIP_RECVMBOX_SIZE, txb, txn,
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
             "yes",
#else
             "NO",
#endif
#if defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)
             "external",
#elif defined(CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC)
             "malloc (<6 KB internal)",
#else
             "INTERNAL",
#endif
             CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    if (!s_pf_req) { s_pf_req = xSemaphoreCreateBinary(); s_pf_done = xSemaphoreCreateBinary(); }
    /* reader on core 0, same priority as the sender: they interleave. Tried on core 1 (2026-10-23): the concurrent
     * 2 MB/s PSRAM memcpy stalls the TLS loop on core 0 (shared PSRAM path/cache) — chunks fell from 435 to 160-270 KB/s. */
    if (!s_reader) task_create_psram(reader_task, "upload_rd", 4096, NULL, 4, &s_reader, 0);
    if (!s_task) task_create_psram(task, "upload", 8192, NULL, 4, &s_task, 0);
    return ESP_OK;
}

esp_err_t upload_start(const char *cmd_id, const char *rec_id, const char *url, const char *token, uint32_t chunk_bytes)
{
    if (!s_task || !cmd_id || !rec_id || !url || !token) return ESP_ERR_INVALID_ARG;
    if (s_busy || store_is_recording()) return ESP_ERR_INVALID_STATE;
    if (!store_find(rec_id)) return ESP_ERR_NOT_FOUND;
    if (strlen(url) >= sizeof s_job.url || strlen(token) >= sizeof s_job.token) return ESP_ERR_INVALID_ARG;
    memset(&s_job, 0, sizeof s_job);
    strncpy(s_job.cmd_id, cmd_id, sizeof s_job.cmd_id - 1);
    strncpy(s_job.rec_id, rec_id, sizeof s_job.rec_id - 1);
    strncpy(s_job.url, url, sizeof s_job.url - 1);
    strncpy(s_job.token, token, sizeof s_job.token - 1);
    s_job.chunk = chunk_bytes ? chunk_bytes : 512 * 1024;
    if (s_job.chunk < MIN_CHUNK) s_job.chunk = MIN_CHUNK;
    if (s_job.chunk > MAX_CHUNK) s_job.chunk = MAX_CHUNK;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool upload_active(void) { return s_busy; }
void upload_get_status(upload_status_t *st) { *st = s_st; if (!s_busy) st->phase = UP_IDLE; }
