/* BLE peripheral + API tunnel — see include/ble.h and docs/ble-v1.md. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ble.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "task_psram.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
void ble_store_config_init(void);                 /* defined in ble_store_config.c, not in its header */
#include "store.h"
#include "imu.h"
#include "webui.h"
#include "nvs.h"

static const char *TAG = "BLE";
#define ADV_ITVL_MS 500
#define HTTP_TIMEOUT_MS 15000

static uint8_t s_addr_type;
static volatile bool s_conn;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static char s_name[32];
static volatile bool s_encrypted;
static bool s_inited;                             /* ble_init done (the host and its bond store exist) */
static int64_t s_pair_until_us;                   /* pairing accepted until then (while no bond exists) */
static esp_timer_handle_t s_kick_timer;           /* delayed disconnect after unpair */

/* ---- pairing policy (0.3.55): one bonded phone, pairing only in the unboxing window ---- */
static int bond_count(void)
{
    ble_addr_t a[2]; int n = 0;
    ble_store_util_bonded_peers(a, &n, 2);
    return n;
}
static bool is_bonded_peer(const ble_addr_t *id)
{
    ble_addr_t a[2]; int n = 0;
    ble_store_util_bonded_peers(a, &n, 2);
    for (int i = 0; i < n; i++) if (ble_addr_cmp(&a[i], id) == 0) return true;
    return false;
}
static int window_left_s(void)
{
    int64_t left = (s_pair_until_us - esp_timer_get_time()) / 1000000;
    return left > 0 ? (int)left : 0;
}
/* Subscriptions (CCCD) are not persisted: a bonded phone subscribing would otherwise write NVS, which is forbidden
 * during a capture session; the app subscribes on every connection anyway. Bonds (SEC) are persisted. */
static int bond_store_write(int obj_type, const union ble_store_value *val)
{
    if (obj_type == BLE_STORE_OBJ_TYPE_CCCD) return 0;
    return ble_store_config_write(obj_type, val);
}
static int bond_store_delete(int obj_type, const union ble_store_key *key)
{
    /* "nothing stored" — NOT 0: ble_store_clear()/ble_store_util_delete_peer() delete in a `while (rc == 0)` loop and
     * answering 0 for a type we never store spins forever (0.3.63: factory_reset from the backend hung the main task) */
    if (obj_type == BLE_STORE_OBJ_TYPE_CCCD) return BLE_HS_ENOENT;
    return ble_store_config_delete(obj_type, key);
}
static const char *addr_str(const ble_addr_t *a, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
    return out;
}

/* ---- GATT: service 7e2c0001-…, REQ 7e2c0002-…, RSP 7e2c0003-… (little-endian in BLE_UUID128_INIT) ---- */
#define P2_UUID(b1, b0) BLE_UUID128_INIT(0x10, 0x6b, 0x0e, 0x1c, 0x7a, 0x2d, 0x5e, 0x9f, 0x3c, 0x4a, 0xf2, 0xb8, b0, b1, 0x2c, 0x7e)
static const ble_uuid128_t s_uuid_svc = P2_UUID(0x00, 0x01);
static const ble_uuid128_t s_uuid_req = P2_UUID(0x00, 0x02);
static const ble_uuid128_t s_uuid_rsp = P2_UUID(0x00, 0x03);
static const ble_uuid128_t s_uuid_live = P2_UUID(0x00, 0x04);
static uint16_t s_req_handle, s_rsp_handle, s_live_handle;
static volatile bool s_rsp_subscribed, s_live_subscribed;
static volatile int64_t s_last_req_us;           /* last tunnel request: the phone's user is busy with the racket (idle.c) */

/* LIVE (0.3.65): raw IMU samples, decimated, one self-contained frame per notification every LIVE_PERIOD_MS:
 *   uint32 seq (index of the first sample, absolute), uint16 n, uint8 div, uint8 sample_bytes, then n x imu_sample_t.
 * div = odr / LIVE_RATE_HZ (4 at 960 Hz -> 240 Hz); consecutive frames: seq' == seq + n*div, else samples were lost. */
#define LIVE_PERIOD_MS 50
#define LIVE_RATE_HZ   240
#define LIVE_MAX_N     25                                /* 8 + 25*20 = 508 B = MTU 512 - 3 - 1 */

/* request assembly (REQ writes arrive on the NimBLE host task) and the API task that serves it */
static uint8_t *s_req;  static size_t s_req_len;  static uint8_t s_req_seq;
static uint8_t *s_rsp;
static SemaphoreHandle_t s_req_ready;             /* given when a complete request has been assembled */
static volatile bool s_busy;                      /* a request is being served */
static uint32_t s_count;

static void advertise(void);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_req_handle) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t frame; os_mbuf_copydata(ctxt->om, 0, 1, &frame);
    uint8_t seq = frame & 0x7F; bool last = frame & 0x80;
    if (seq == 0) { s_req_len = 0; s_req_seq = 0; }
    if (seq != s_req_seq) { ESP_LOGW(TAG, "req chunk %u out of order (expected %u): dropped", seq, s_req_seq); s_req_len = 0; s_req_seq = 0; return 0; }
    if (s_busy) { if (last) ESP_LOGW(TAG, "request while busy: dropped"); s_req_len = 0; s_req_seq = 0; return 0; }
    size_t n = len - 1;
    if (s_req_len + n >= BLE_MSG_MAX) { ESP_LOGW(TAG, "request > %d B: dropped", BLE_MSG_MAX); s_req_len = 0; s_req_seq = 0; return 0; }
    os_mbuf_copydata(ctxt->om, 1, n, s_req + s_req_len);
    s_req_len += n; s_req_seq = (s_req_seq + 1) & 0x7F;
    if (last) { s_req[s_req_len] = 0; s_busy = true; xSemaphoreGive(s_req_ready); }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_uuid_req.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC, .val_handle = &s_req_handle },
          { .uuid = &s_uuid_rsp.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_rsp_handle },
          { .uuid = &s_uuid_live.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_live_handle },
          { 0 } } },
    { 0 },
};

/* ---- notify a whole message in frames ---- */
static int send_msg(const uint8_t *msg, size_t len)
{
    size_t chunk = (size_t)s_mtu - 3 - 1, off = 0; uint8_t seq = 0;
    if (chunk < 4) chunk = 19;
    do {
        size_t n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_att_pkt();
        if (!om) return -1;
        uint8_t frame = seq | (off + n >= len ? 0x80 : 0);
        os_mbuf_append(om, &frame, 1); os_mbuf_append(om, msg + off, n);
        int rc = ble_gatts_notify_custom(s_conn_handle, s_rsp_handle, om);   /* consumes om */
        if (rc != 0) { ESP_LOGW(TAG, "notify rc %d at %u/%u", rc, (unsigned)off, (unsigned)len); return rc; }
        off += n; seq = (seq + 1) & 0x7F;
    } while (off < len);
    return 0;
}

static void answer(int id, int status, const char *body_json, const char *err)
{
    int n;
    if (body_json) n = snprintf((char *)s_rsp, BLE_MSG_MAX, "{\"id\":%d,\"s\":%d,\"b\":%s}", id, status, body_json);
    else           n = snprintf((char *)s_rsp, BLE_MSG_MAX, "{\"id\":%d,\"s\":%d,\"b\":{\"error\":\"%s\"}}", id, status, err ? err : "error");
    if (n >= BLE_MSG_MAX) { n = snprintf((char *)s_rsp, BLE_MSG_MAX, "{\"id\":%d,\"s\":507,\"b\":{\"error\":\"response too large\"}}", id); status = 507; }
    if (s_conn && s_rsp_subscribed) send_msg(s_rsp, n);
}

/* ---- the HTTP exchange over loopback ---- */
/* Returns the HTTP status (or <0), body (NUL-terminated, in s_rsp + offset so that answer() can wrap it in place). */
static int http_loopback(const char *method, const char *path, const char *body, char *out, size_t out_max, bool *json)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT_MS / 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -2; }
    char hdr[256];
    size_t blen = body ? strlen(body) : 0;
    int hn = snprintf(hdr, sizeof hdr, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nX-Transport: ble\r\n%s%s%zu\r\n\r\n",
                      method, path, blen ? "Content-Type: application/json\r\n" : "", "Content-Length: ", blen);
    if (send(fd, hdr, hn, 0) < 0 || (blen && send(fd, body, blen, 0) < 0)) { close(fd); return -3; }
    /* Read the headers, then exactly Content-Length bytes: esp_http_server ignores "Connection: close" and keeps the
     * socket open, so reading until EOF would wait the full receive timeout on every request (0.3.54). Replies
     * without Content-Length (chunked: the file downloads, excluded over BLE) are read until close/timeout. */
    size_t got = 0, want = 0; int r; char *sep = NULL;
    while (got < out_max - 1 && (r = recv(fd, out + got, out_max - 1 - got, 0)) > 0) {
        got += r; out[got] = 0;
        if (!sep) {
            sep = strstr(out, "\r\n\r\n");
            if (!sep) continue;
            const char *cl = strcasestr(out, "\r\nContent-Length:");
            if (!cl || cl > sep) continue;                  /* no length: read to close */
            want = (size_t)(sep + 4 - out) + strtoul(cl + 17, NULL, 10);
        }
        if (want && got >= want) break;
    }
    close(fd);
    out[got] = 0;
    if (got < 12 || strncmp(out, "HTTP/1.", 7) != 0) return -4;
    int status = atoi(out + 9);
    if (!sep) return -5;
    *json = strstr(out, "application/json") != NULL && strstr(out, "application/json") < sep;
    memmove(out, sep + 4, got - (sep + 4 - out) + 1);   /* body to the front */
    return status;
}

/* ---- LIVE sender: ring (webui.c, fed by the core-1 sampler) -> decimate -> one notification per period ---- */
static void live_task(void *arg)
{
    webui_sample_t *buf = heap_caps_malloc(64 * sizeof(webui_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *frame = heap_caps_malloc(8 + LIVE_MAX_N * sizeof(webui_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !frame) { ESP_LOGE(TAG, "no PSRAM for the live stream"); vTaskDelete(NULL); }
    uint32_t rd = webui_ring_wr(); bool was_on = false; uint32_t sent = 0, dropped = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LIVE_PERIOD_MS));
        bool on = s_conn && s_live_subscribed;
        if (!on) { if (was_on) { ESP_LOGI(TAG, "LIVE: %u frames sent, %u not delivered", (unsigned)sent, (unsigned)dropped); sent = dropped = 0; } was_on = false; rd = webui_ring_wr(); continue; }
        was_on = true;
        int odr = imu_get_cfg()->odr_hz;                                   /* the ACTIVE rate */ uint32_t div = odr > LIVE_RATE_HZ ? (uint32_t)(odr / LIVE_RATE_HZ) : 1;
        uint32_t max_n = ((uint32_t)s_mtu - 3 - 8) / sizeof(webui_sample_t);
        if (max_n > LIVE_MAX_N) max_n = LIVE_MAX_N;
        if (max_n == 0) { rd = webui_ring_wr(); continue; }                   /* MTU 23: no room, stay silent */
        uint32_t n = 0, first = 0;
        while (n < max_n) {                                                   /* drain the ring, keep every div-th sample */
            uint32_t got = webui_ring_read(&rd, buf, 64);
            if (!got) break;
            for (uint32_t k = 0; k < got && n < max_n; k++) {
                uint32_t idx = rd - got + k;
                if (idx % div) continue;
                if (!n) first = idx;
                memcpy(frame + 8 + n * sizeof(webui_sample_t), &buf[k], sizeof(webui_sample_t)); n++;
            }
        }
        if (!n) continue;
        uint16_t n16 = (uint16_t)n; uint8_t d8 = (uint8_t)div, sb = (uint8_t)sizeof(webui_sample_t);
        memcpy(frame, &first, 4); memcpy(frame + 4, &n16, 2); frame[6] = d8; frame[7] = sb;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, 8 + n * sizeof(webui_sample_t));
        if (!om || ble_gatts_notify_custom(s_conn_handle, s_live_handle, om) != 0) dropped++; else sent++;
    }
}

static void api_task(void *arg)
{
    char *http = heap_caps_malloc(BLE_MSG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        xSemaphoreTake(s_req_ready, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        s_last_req_us = t0;
        cJSON *j = cJSON_ParseWithLength((char *)s_req, s_req_len);
        cJSON *jid = j ? cJSON_GetObjectItem(j, "id") : NULL, *jm = j ? cJSON_GetObjectItem(j, "m") : NULL,
              *jp = j ? cJSON_GetObjectItem(j, "p") : NULL, *jb = j ? cJSON_GetObjectItem(j, "b") : NULL;
        int id = cJSON_IsNumber(jid) ? (int)jid->valuedouble : 0;
        if (!j || !cJSON_IsString(jm) || !cJSON_IsString(jp) || jp->valuestring[0] != '/' || strlen(jp->valuestring) > 200) {
            answer(id, 400, NULL, "bad request: need id, m, p"); 
        } else if (!http) {
            answer(id, 500, NULL, "no memory");
        } else {
            char *body = jb ? cJSON_PrintUnformatted(jb) : NULL;
            bool json = false;
            int st = http_loopback(jm->valuestring, jp->valuestring, body, http, BLE_MSG_MAX, &json);
            if (st < 0) answer(id, 502, NULL, "web server unreachable");
            else if (!json && http[0]) answer(id, 415, NULL, "not JSON over BLE");
            else answer(id, st, http[0] ? http : "null", NULL);
            ESP_LOGI(TAG, "req #%" PRIu32 " %s %s -> %d (%u B, %lld ms)", ++s_count, jm->valuestring, jp->valuestring, st < 0 ? 502 : st,
                     (unsigned)strlen(http), (esp_timer_get_time() - t0) / 1000);
            free(body);
        }
        cJSON_Delete(j);
        s_busy = false;
    }
}

/* ---- GAP ---- */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = true; s_conn_handle = ev->connect.conn_handle; s_rsp_subscribed = false; s_encrypted = false; s_busy = false; s_req_len = 0; s_req_seq = 0;
            struct ble_gap_conn_desc d; char as[18];
            ble_gap_conn_find(ev->connect.conn_handle, &d);
            ESP_LOGI(TAG, "phone %s connected (handle %d), %u KB internal free", addr_str(&d.peer_id_addr, as), ev->connect.conn_handle,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
            /* who may stay: the bonded phone; or, with no bond yet, anyone inside the pairing window (not while recording:
             * storing the bond writes NVS) */
            if (bond_count() == 0) {
                if (window_left_s() == 0) { ESP_LOGW(TAG, "not paired and the pairing window is closed: disconnecting (power-cycle the racket to pair)"); ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); break; }
                if (store_is_recording()) { ESP_LOGW(TAG, "pairing refused while recording: disconnecting"); ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); break; }
                ESP_LOGI(TAG, "pairing with this phone (window open, %d s left)", window_left_s());
            } else if (!is_bonded_peer(&d.peer_id_addr)) {
                ESP_LOGW(TAG, "not the paired phone: disconnecting (factory reset to pair another phone)");
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); break;
            }
            int rc = ble_gap_security_initiate(ev->connect.conn_handle);   /* encrypt with the bond, or start pairing */
            if (rc != 0) { ESP_LOGW(TAG, "security initiate rc %d: disconnecting", rc); ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); }
        } else { ESP_LOGW(TAG, "connect failed (status %d): advertising again", ev->connect.status); advertise(); }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = false; s_conn_handle = BLE_HS_CONN_HANDLE_NONE; s_rsp_subscribed = false; s_live_subscribed = false; s_encrypted = false; s_mtu = 23;
        ESP_LOGI(TAG, "phone disconnected (reason 0x%03x): advertising again", ev->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ev->enc_change.status != 0 || ble_gap_conn_find(ev->enc_change.conn_handle, &d) != 0) {
            ESP_LOGW(TAG, "pairing/encryption failed (status %d): disconnecting", ev->enc_change.status);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM); break;
        }
        s_encrypted = d.sec_state.encrypted;
        ESP_LOGI(TAG, "link encrypted (bonded %d, %s); %d bond(s) stored", d.sec_state.bonded, d.sec_state.authenticated ? "authenticated" : "just works", bond_count());
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* the paired phone lost its key (user "forgot" the racket): let it pair again, unless recording (NVS write) */
        struct ble_gap_conn_desc d;
        if (store_is_recording() || ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) != 0) {
            ESP_LOGW(TAG, "paired phone asks to re-pair while recording: refused");
            ble_gap_terminate(ev->repeat_pairing.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ESP_LOGW(TAG, "paired phone asks to re-pair: replacing its bond");
        ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE: if (!s_conn) advertise(); break;
    case BLE_GAP_EVENT_MTU: s_mtu = ev->mtu.value; ESP_LOGI(TAG, "MTU %d (chunks of %d B)", s_mtu, s_mtu - 4); break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_rsp_handle) { s_rsp_subscribed = ev->subscribe.cur_notify; ESP_LOGI(TAG, "RSP notifications %s", s_rsp_subscribed ? "on" : "off"); }
        if (ev->subscribe.attr_handle == s_live_handle) { s_live_subscribed = ev->subscribe.cur_notify; ESP_LOGI(TAG, "LIVE stream %s", s_live_subscribed ? "on" : "off"); }
        break;
    default: break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name; f.name_len = strlen(s_name); f.name_is_complete = 1;   /* 3 + 2 + 22 = 27 of 31 B */
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields: rc %d", rc); return; }
    struct ble_hs_adv_fields r = { 0 };                 /* the service UUID goes in the scan response (18 B) */
    r.uuids128 = (ble_uuid128_t *)&s_uuid_svc; r.num_uuids128 = 1; r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MS);
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start: rc %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "address: rc %d", rc); return; }
    uint8_t a[6] = { 0 }; ble_hs_id_copy_addr(s_addr_type, a, NULL);
    ESP_LOGI(TAG, "advertising as '%s' every %d ms, address %02x:%02x:%02x:%02x:%02x:%02x", s_name, ADV_ITVL_MS, a[5], a[4], a[3], a[2], a[1], a[0]);
    advertise();
}
static void on_reset(int reason) { ESP_LOGW(TAG, "host reset (reason %d)", reason); }
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }

esp_err_t ble_init(const char *device_id)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), before_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(s_name, sizeof s_name, "Perceptio-%s", device_id ? device_id : "?");
    s_req = heap_caps_malloc(BLE_MSG_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rsp = heap_caps_malloc(BLE_MSG_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_req_ready = xSemaphoreCreateBinary();
    if (!s_req || !s_rsp || !s_req_ready) return ESP_ERR_NO_MEM;
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e)); return e; }
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = on_reset;
    /* LE Secure Connections, Just Works (no display/keypad on a racket), bonding, 1 bond (sdkconfig) */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_mitm = 0; ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();                                   /* bonds in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST) */
    ble_hs_cfg.store_write_cb = bond_store_write; ble_hs_cfg.store_delete_cb = bond_store_delete;
    s_pair_until_us = (int64_t)BLE_PAIR_WINDOW_S * 1000000;
    ble_svc_gap_init(); ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs); if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatt services: rc %d", rc); return ESP_FAIL; }
    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(host_task);
    task_create_psram(api_task, "ble_api", 6144, NULL, 4, NULL, 0);   /* sockets + cJSON only: PSRAM stack is fine */
    task_create_psram(live_task, "ble_live", 4096, NULL, 5, NULL, 0);  /* LIVE frames (0.3.65) */
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), after_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "stack up: cost %d KB internal, %d KB PSRAM; %u KB internal free", (int)((before - after) >> 10), (int)((before_ps - after_ps) >> 10), (unsigned)(after >> 10));
    s_inited = true;
    return ESP_OK;
}

bool ble_connected(void) { return s_conn; }
bool ble_live_active(void) { return s_conn && s_live_subscribed; }
int64_t ble_last_request_us(void) { return s_last_req_us; }

void ble_status(ble_status_t *st)
{
    if (!s_inited) { memset(st, 0, sizeof *st); return; }
    st->bonded = bond_count() > 0;
    st->pairing_open = !st->bonded && window_left_s() > 0;
    st->window_s = st->bonded ? 0 : window_left_s();
    st->connected = s_conn; st->encrypted = s_encrypted;
}

static void kick_cb(void *arg) { if (s_conn) ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM); }

esp_err_t ble_unpair(void)
{
    if (!s_inited) {                                           /* before ble_init (factory gesture at boot): wipe NimBLE's
                                                                * NVS namespace; the host reads it later at start-up */
        nvs_handle_t h; esp_err_t e = nvs_open("nimble_bond", NVS_READWRITE, &h);
        if (e == ESP_OK) { e = nvs_erase_all(h); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); }
        ESP_LOGW(TAG, "unpaired before BLE start: bond store erased (%s)", esp_err_to_name(e));
        return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
    }
    if (store_is_recording()) return ESP_ERR_INVALID_STATE;
    int n = bond_count();
    int rc = ble_store_clear();
    s_pair_until_us = esp_timer_get_time() + (int64_t)BLE_PAIR_WINDOW_S * 1000000;
    ESP_LOGW(TAG, "unpaired: %d bond(s) erased (rc %d), pairing window open for %d s", n, rc, BLE_PAIR_WINDOW_S);
    if (s_conn) {                                              /* let the answer go out first, then drop the phone */
        if (!s_kick_timer) { esp_timer_create_args_t a = { .callback = kick_cb, .name = "ble_kick" }; esp_timer_create(&a, &s_kick_timer); }
        esp_timer_start_once(s_kick_timer, 500000);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
