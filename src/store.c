#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "store.h"
#include "board.h"
#include "config.h"
#include "clock.h"
#include "sampler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "task_psram.h"
#include <math.h>
#include "mbedtls/sha256.h"
#include "pcr2_codec1.h"
#include "cJSON.h"

static const char *TAG = "STORE";

#define MAX_REC     32
#define RING_BLOCKS 16                      /* raw 400-sample blocks in PSRAM: ~6.9 s of slack at 927 Hz */
#define MAP_FREE    0xFF
#define MAP_BAD     0xFE

/* producer -> writer unit: one raw block (or the END record) */
typedef struct {
    int64_t  first_us;
    uint32_t first_idx;
    uint16_t n;                             /* samples (block) or bytes (END record) */
    uint8_t  flags;                         /* bit0: samples lost before this block */
    uint8_t  kind;                          /* 0 block, 1 END2 record in data[] */
    uint8_t  data[PCR2_SPB * PCR2_SAMPLE_BYTES];
} raw_blk_t;

static const uint8_t  FIELD_SIZE[PCR2_FIELDS] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static const uint16_t FIELD_OFF[PCR2_FIELDS]  = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18 };

/* ------------------------------------------------------------------ */
static char      s_devid[16];
static uint32_t  s_boot_id;
static uint8_t   s_rec_n;                   /* per-boot counter */
static bool      s_nand_ok;
static store_rec_t s_rec[MAX_REC];
static uint8_t   s_map[NAND_BLOCKS];        /* block -> inventory slot / FREE / BAD */
static uint16_t  s_ord[NAND_BLOCKS];        /* block -> ordinal within its recording (page/64) */
static SemaphoreHandle_t s_ctl;             /* serialises control calls (core 0) */
static store_event_fn s_hook;

/* capture state (producer = core-1 sampler) */
static volatile store_state_t s_state = REC_IDLE;
static volatile bool s_feed_on;
static int s_cur;                           /* inventory slot of the running recording */
static raw_blk_t *s_ring;                   /* RING_BLOCKS raw blocks, PSRAM */
static volatile uint32_t s_wr, s_rd;        /* raw block counters (producer / writer) */
static uint32_t  s_fill;                    /* samples in the raw block being filled */
static uint32_t  s_samples, s_dropped, s_saturated;
/* Gaps: an interval > 1.5 sample periods means at least one data-ready edge was merged with the next one (the sampler
 * reads the IMU output registers, no FIFO), so that sample was never read. Neither the sampler nor `dropped` (ring full)
 * can see it; the timestamps can. 0.3.26, after the backend found one 3.2 ms gap in d40592485fcc-26-0. */
static uint32_t  s_gaps, s_gap_max_us, s_gap_thr_us;
static int64_t   s_first_mono, s_last_mono, s_prev_us;
static bool      s_lost_pending;
static volatile uint32_t s_backlog_max;
static TaskHandle_t s_writer;

/* writer-side state (core 1): encoder, page assembler, block allocation */
static uint8_t  *s_enc;                     /* 32 + worst-case encoded block */
static uint8_t  *s_pg;                      /* page assembler (2048 B, PSRAM) */
static uint32_t  s_pg_fill, s_pg_idx, s_pg_off;   /* stream bytes in the page, page index, stream offset of the page */
static uint32_t  s_blocks;                  /* blocks written */
static int       s_wblk = -1, s_wpg, s_wblk_next = -1;
static uint16_t  s_alloc_cursor = STORE_FIRST_BLK;
static volatile bool s_full;
static int       s_skip_ord = -1;
static uint32_t  s_lost_pages;
static mbedtls_sha256_context s_sha;        /* over the stream, in writer order */
static volatile bool s_sha_done;

/* ------------------------------------------------------------------ */
static float board_temp_c(void) { sampler_stats_t st; sampler_get_stats(&st); return 25.0f + st.last.v[0] / 256.0f; }
static float measured_rate_hz(void) { sampler_stats_t st; sampler_get_stats(&st); return st.avg_hz > 100 ? st.avg_hz : (float)st.rate_hz; }
static inline raw_blk_t *ring_blk(uint32_t i) { return &s_ring[i % RING_BLOCKS]; }
static void make_id(char *out, uint32_t boot, uint8_t n) { snprintf(out, 32, "%s-%" PRIu32 "-%u", s_devid, boot, n); }

static int slot_find(uint32_t boot, uint8_t n)
{
    for (int i = 0; i < MAX_REC; i++) if (s_rec[i].used && s_rec[i].boot_id == boot && s_rec[i].rec_n == n) return i;
    return -1;
}
static int slot_alloc(void)
{
    for (int i = 0; i < MAX_REC; i++) if (!s_rec[i].used) { memset(&s_rec[i], 0, sizeof s_rec[i]); s_rec[i].used = true; return i; }
    return -1;
}
static uint32_t free_blocks(void)
{
    uint32_t n = 0;
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) if (s_map[b] == MAP_FREE) n++;
    return n;
}
static int block_take(int slot, uint16_t ord)                /* erase a free block (wear rotation), map it */
{
    for (int tries = 0; tries < NAND_BLOCKS; tries++) {
        uint16_t b = s_alloc_cursor;
        s_alloc_cursor = (b >= STORE_LAST_BLK) ? STORE_FIRST_BLK : b + 1;
        if (s_map[b] != MAP_FREE) continue;
        esp_err_t e = nand_block_erase(b);
        if (e != ESP_OK) { ESP_LOGW(TAG, "erase block %u failed (%s): marking bad", b, esp_err_to_name(e)); nand_mark_bad(b); s_map[b] = MAP_BAD; continue; }
        s_map[b] = slot; s_ord[b] = ord;
        return b;
    }
    return -1;
}
static int block_of(int slot, uint16_t ord)
{
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) if (s_map[b] == slot && s_ord[b] == ord) return b;
    return -1;
}
static bool hdr_valid(const store_page_hdr_t *h) { return h->magic == STORE_MAGIC && STORE_PG_TYPE(h->type) >= 1 && STORE_PG_TYPE(h->type) <= 3 && h->len <= STORE_PAYLOAD; }

static esp_err_t read_hdr(int slot, uint32_t idx, store_page_hdr_t *h)
{
    int b = block_of(slot, idx / NAND_PAGES_PER_BLK);
    if (b < 0) return ESP_ERR_NOT_FOUND;
    esp_err_t e = nand_page_read((uint32_t)b * NAND_PAGES_PER_BLK + idx % NAND_PAGES_PER_BLK, 0, (uint8_t *)h, sizeof *h, NULL);
    if (e == ESP_OK && !hdr_valid(h)) e = ESP_ERR_NOT_FOUND;
    return e;
}
static esp_err_t read_page(int slot, uint32_t idx, uint8_t *pg2048)
{
    int b = block_of(slot, idx / NAND_PAGES_PER_BLK);
    if (b < 0) return ESP_ERR_NOT_FOUND;
    int ecc = 0;
    esp_err_t e = nand_page_read((uint32_t)b * NAND_PAGES_PER_BLK + idx % NAND_PAGES_PER_BLK, 0, pg2048, NAND_PAGE_SIZE, &ecc);
    if (ecc == 1) ESP_LOGW(TAG, "%s page %" PRIu32 ": ECC corrected", s_rec[slot].rec_id, idx);
    if (e == ESP_OK && !hdr_valid((store_page_hdr_t *)pg2048)) e = ESP_ERR_NOT_FOUND;
    return e;
}

/* ------------------------------------------------------------------ */
/* writer task — core 1, below the sampler                             */
/* ------------------------------------------------------------------ */
static void program_page(uint8_t *pg, uint32_t idx)      /* block allocation, pre-erase ahead, bad-block handling */
{
    uint16_t ord = idx / NAND_PAGES_PER_BLK;
    if ((int)ord == s_skip_ord) { s_lost_pages++; return; }
    if (s_wblk < 0 || s_ord[s_wblk] != ord || s_wpg >= NAND_PAGES_PER_BLK) {
        if (s_wblk_next >= 0) { s_wblk = s_wblk_next; s_ord[s_wblk] = ord; s_wblk_next = -1; }
        else s_wblk = block_take(s_cur, ord);
        s_wpg = 0;
        if (s_wblk < 0) { s_full = true; s_lost_pages++; ESP_LOGE(TAG, "storage full: page %" PRIu32 " dropped", idx); return; }
        s_wblk_next = block_take(s_cur, ord + 1);        /* pre-erase ahead; none left -> the control side stops */
        if (s_wblk_next < 0) s_full = true;
    }
    uint32_t page = (uint32_t)s_wblk * NAND_PAGES_PER_BLK + s_wpg;
    esp_err_t e = nand_page_program(page, pg, NAND_PAGE_SIZE);
    s_wpg++;
    if (e != ESP_OK) {                                   /* P-FAIL: drop the rest of this 64-page ordinal, keep going */
        ESP_LOGE(TAG, "program page %" PRIu32 " failed (%s): block %d marked bad", page, esp_err_to_name(e), s_wblk);
        nand_mark_bad(s_wblk); s_map[s_wblk] = MAP_BAD;
        s_wblk = -1; s_wpg = 0; s_skip_ord = ord; s_lost_pages++;
    }
}

/* flush the assembler page: container header, running hash, (digest on the END page), program */
static void page_flush(uint8_t type)
{
    store_page_hdr_t h = { .magic = STORE_MAGIC, .type = type, .rec_n = s_rec[s_cur].rec_n, .boot_id = s_rec[s_cur].boot_id,
                           .page = s_pg_idx, .len = s_pg_fill, .offset = s_pg_off };
    mbedtls_sha256_update(&s_sha, s_pg + STORE_HDR_BYTES, s_pg_fill);
    if (type == STORE_PG_END && s_pg_fill <= STORE_SHA_OFF) {
        mbedtls_sha256_finish(&s_sha, s_pg + STORE_HDR_BYTES + STORE_SHA_OFF);
        memcpy(s_rec[s_cur].sha256, s_pg + STORE_HDR_BYTES + STORE_SHA_OFF, 32);
        h.type |= STORE_PG_HAS_SHA;
        s_sha_done = true;
    }
    memcpy(s_pg, &h, sizeof h);
    program_page(s_pg, s_pg_idx);
    s_pg_idx++; s_pg_off += s_pg_fill; s_pg_fill = 0;
    memset(s_pg + STORE_HDR_BYTES, 0xFF, STORE_PAYLOAD);
}

static void stream_append(const uint8_t *d, uint32_t n)
{
    while (n) {
        uint32_t k = STORE_PAYLOAD - s_pg_fill; if (k > n) k = n;
        memcpy(s_pg + STORE_HDR_BYTES + s_pg_fill, d, k);
        s_pg_fill += k; d += k; n -= k;
        if (s_pg_fill == STORE_PAYLOAD) page_flush(STORE_PG_BLOCK);
    }
}

static void write_raw(raw_blk_t *rb)
{
    if (rb->kind == 1) {                                 /* END2 record, then close the file */
        stream_append(rb->data, rb->n);
        if (s_pg_fill) page_flush(STORE_PG_END);         /* digest inside when it fits (fill ≤ 2000) ... */
        if (!s_sha_done) page_flush(STORE_PG_END);       /* ... otherwise one extra END page (len 0) carries it */
        return;
    }
    if (rb->n == 0) return;
    size_t enc = pcr2_codec1_encode(rb->data, rb->n, PCR2_SAMPLE_BYTES, FIELD_SIZE, FIELD_OFF, PCR2_FIELDS, s_enc + PCR2_BLOCK_HDR);
    pcr2_block_hdr_t bh = { .magic = { 'B', 'L', 'K', '2' }, .block_seq = s_blocks, .first_mono_us = (uint64_t)rb->first_us,
                            .first_sample_index = rb->first_idx, .n_samples = rb->n, .codec = PCR2_CODEC, .flags = rb->flags,
                            .payload_len = (uint32_t)enc, .crc32 = esp_rom_crc32_le(0, s_enc + PCR2_BLOCK_HDR, enc) };
    memcpy(s_enc, &bh, sizeof bh);
    stream_append(s_enc, PCR2_BLOCK_HDR + (uint32_t)enc);
    s_blocks++;
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        while (s_rd != s_wr) {
            uint32_t backlog = s_wr - s_rd;
            if (backlog > s_backlog_max) s_backlog_max = backlog;
            write_raw(ring_blk(s_rd));
            __atomic_store_n(&s_rd, s_rd + 1, __ATOMIC_RELEASE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* producer side (core-1 sampler)                                      */
/* ------------------------------------------------------------------ */
static void raw_commit(void)
{
    __atomic_store_n(&s_wr, s_wr + 1, __ATOMIC_RELEASE);
    if (s_writer) xTaskNotifyGive(s_writer);
}

static void block_close(void)
{
    raw_blk_t *rb = ring_blk(s_wr);
    rb->n = s_fill; rb->flags = s_lost_pending ? 1 : 0; rb->kind = 0;
    s_lost_pending = false;
    raw_commit();
    s_fill = 0;
}

static volatile bool s_synth;               /* a synthetic recording is being generated: the IMU feed is ignored */

static void feed_impl(const imu_sample_t *s, int64_t t_us);
void store_feed(const imu_sample_t *s, int64_t t_us)
{
    if (!s_feed_on || s_synth) return;
    feed_impl(s, t_us);
}
static void feed_impl(const imu_sample_t *s, int64_t t_us)
{
    if (s_wr - __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE) >= RING_BLOCKS) { s_dropped++; s_lost_pending = true; return; }
    if (s_samples && t_us - s_prev_us > (int64_t)s_gap_thr_us) {
        uint32_t g = (uint32_t)(t_us - s_prev_us);
        s_gaps++; if (g > s_gap_max_us) s_gap_max_us = g;
        s_lost_pending = true;                            /* "samples lost" flag on the block holding the gap */
    }
    if (s_fill && (t_us - s_prev_us > PCR2_MAX_DT_US || t_us < s_prev_us)) block_close();   /* dt_us is u16: gap -> new block */
    raw_blk_t *rb = ring_blk(s_wr);
    if (s_fill == 0) { rb->first_us = t_us; rb->first_idx = s_samples; }
    uint8_t *p = rb->data + s_fill * PCR2_SAMPLE_BYTES;
    uint16_t dt = s_fill ? (uint16_t)(t_us - s_prev_us) : 0;
    memcpy(p, &dt, 2);
    memcpy(p + 2, &s->v[1], 9 * sizeof(int16_t));         /* GX GY GZ AX AY AZ HGX HGY HGZ (temperature is not logged) */
    if (s->v[4] == INT16_MAX || s->v[4] == INT16_MIN || s->v[5] == INT16_MAX || s->v[5] == INT16_MIN ||
        s->v[6] == INT16_MAX || s->v[6] == INT16_MIN) s_saturated++;
    if (s_samples == 0) s_first_mono = t_us;
    s_last_mono = s_prev_us = t_us;
    s_fill++; s_samples++;
    if (s_fill == PCR2_SPB) block_close();
}

/* ------------------------------------------------------------------ */
/* control (core 0)                                                    */
/* ------------------------------------------------------------------ */
static void fill_live(store_rec_t *r)
{
    r->samples = s_samples; r->dropped = s_dropped; r->saturated = s_saturated; r->blocks = s_blocks; r->gaps = s_gaps;
    r->pages = s_pg_idx + (s_pg_fill ? 1 : 0);
    r->bytes = s_pg_off + s_pg_fill;
}

/* PCR2 file header JSON (data-v2.md §1.1), padded with spaces so the header fills page 0 exactly */
static int build_file_header(uint8_t *stream, const store_rec_t *r, const char *by)
{
    const imu_cfg_t *ic = imu_get_cfg(); imu_scale_t sc = imu_get_scale();
    char *j = (char *)stream + 12;
    int cap = STORE_PAYLOAD - 12;
    int n = snprintf(j, cap,
        "{\"rec_id\":\"%s\",\"device_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"rec_tag\":\"%s\",\"fw\":\"%s\",\"hw_rev\":\"%d\",\"started_by\":\"%s\","
        "\"start_anchor\":{\"mono_us\":%lld,\"utc_ms\":%lld,\"synced\":%s},\"nominal_rate_hz\":%d,"
        "\"sample\":{\"size\":%d,\"fields\":["
        "{\"name\":\"dt_us\",\"type\":\"u16\",\"unit\":\"us\"},"
        "{\"name\":\"gx\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"dps\",\"sensor\":\"gyro\"},"
        "{\"name\":\"gy\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"dps\",\"sensor\":\"gyro\"},"
        "{\"name\":\"gz\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"dps\",\"sensor\":\"gyro\"},"
        "{\"name\":\"a1x\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_16g\"},"
        "{\"name\":\"a1y\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_16g\"},"
        "{\"name\":\"a1z\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_16g\"},"
        "{\"name\":\"a2x\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_320g\"},"
        "{\"name\":\"a2y\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_320g\"},"
        "{\"name\":\"a2z\",\"type\":\"i16\",\"scale\":%.9g,\"unit\":\"g\",\"sensor\":\"accel_320g\"}]},"
        "\"sensors\":{\"gyro\":{\"part\":\"LSM6DSV320X\",\"fsr_dps\":%d,\"odr_hz\":%d},"
        "\"accel_16g\":{\"part\":\"LSM6DSV320X\",\"fsr_g\":%d,\"odr_hz\":%d},"
        "\"accel_320g\":{\"part\":\"LSM6DSV320X\",\"fsr_g\":%d,\"odr_hz\":%d}},"
        "\"axes\":\"LSM6DSV320X sensor frame as on the datasheet; racket-frame mapping to be calibrated\","
        "\"block_samples\":%d,\"codec\":%d,\"odr_measured_hz\":%.0f,\"temp_c_start\":%.1f}",
        r->rec_id, s_devid, r->boot_id, r->rec_tag, config_fw_version(), PERCEPTIO_HW_REV, by,
        (long long)r->start_mono_us, (long long)r->start_utc_ms, r->start_utc_ms ? "true" : "false",
        measured_rate_hz() > 100 ? (int)(measured_rate_hz() + 0.5f) : ic->odr_hz,   /* the real rate (~927): the ODR setting is in sensors.*.odr_hz */
        PCR2_SAMPLE_BYTES,
        sc.gy_mdps_lsb / 1000.0, sc.gy_mdps_lsb / 1000.0, sc.gy_mdps_lsb / 1000.0,
        sc.xl_mg_lsb / 1000.0, sc.xl_mg_lsb / 1000.0, sc.xl_mg_lsb / 1000.0,
        sc.hg_mg_lsb / 1000.0, sc.hg_mg_lsb / 1000.0, sc.hg_mg_lsb / 1000.0,
        ic->gy_fs_dps, ic->odr_hz, ic->xl_fs_g, ic->odr_hz, ic->hg_fs_g, ic->odr_hz,
        PCR2_SPB, PCR2_CODEC, measured_rate_hz(), board_temp_c());
    if (n < 0 || n >= cap) return -1;
    memset(j + n, ' ', cap - n);                           /* whitespace is valid JSON: pad to fill the page */
    memcpy(stream, "PCR2", 4);
    uint16_t v = 1, z = 0; uint32_t jl = cap;
    memcpy(stream + 4, &v, 2); memcpy(stream + 6, &z, 2); memcpy(stream + 8, &jl, 4);
    return STORE_PAYLOAD;
}

esp_err_t store_start(const char *rec_tag, const char *by, char rec_id_out[32])
{
    if (!s_nand_ok || !s_ring) return ESP_FAIL;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (s_state != REC_IDLE) { xSemaphoreGive(s_ctl); return ESP_ERR_INVALID_STATE; }
    if (free_blocks() < 2 || s_rec_n == 255) { xSemaphoreGive(s_ctl); return ESP_ERR_NO_MEM; }
    int slot = slot_alloc();
    if (slot < 0) { xSemaphoreGive(s_ctl); ESP_LOGW(TAG, "inventory full (%d recordings): delete some", MAX_REC); return ESP_ERR_NO_MEM; }

    clock_resync();                                   /* spec §11: SNTP at start when the network is up (async) */
    config_set_locked(true);                          /* no NVS writes during a session */

    store_rec_t *r = &s_rec[slot];
    r->boot_id = s_boot_id; r->rec_n = s_rec_n++; r->this_boot = true;
    make_id(r->rec_id, r->boot_id, r->rec_n);
    if (rec_tag) strncpy(r->rec_tag, rec_tag, sizeof r->rec_tag - 1);
    r->start_mono_us = esp_timer_get_time(); r->start_utc_ms = clock_utc_ms();
    r->odr_hz = imu_get_cfg()->odr_hz;

    s_cur = slot; s_wr = s_rd = 0; s_fill = 0; s_blocks = 0;
    s_samples = s_dropped = s_saturated = 0; s_first_mono = s_last_mono = s_prev_us = 0; s_lost_pending = false;
    s_backlog_max = 0; s_full = false; s_wblk = -1; s_wblk_next = -1; s_wpg = 0; s_skip_ord = -1; s_lost_pages = 0;
    { float hz = measured_rate_hz(); if (hz < 100) hz = (float)imu_get_cfg()->odr_hz;
      s_gaps = s_gap_max_us = 0; s_gap_thr_us = (uint32_t)(1.5e6f / hz); }
    s_pg_fill = 0; s_pg_idx = 0; s_pg_off = 0; memset(s_pg + STORE_HDR_BYTES, 0xFF, STORE_PAYLOAD);
    mbedtls_sha256_init(&s_sha); mbedtls_sha256_starts(&s_sha, 0); s_sha_done = false;

    /* page 0 = file header, written now (the writer is idle) */
    int len = build_file_header(s_pg + STORE_HDR_BYTES, r, by ? by : "");
    if (len < 0) { r->used = false; config_set_locked(false); xSemaphoreGive(s_ctl); ESP_LOGE(TAG, "file header does not fit a page"); return ESP_FAIL; }
    s_pg_fill = len;
    page_flush(STORE_PG_FILEHDR);

    s_state = REC_RECORDING;
    s_feed_on = true;
    if (rec_id_out) strncpy(rec_id_out, r->rec_id, 32);
    /* sample clock = esp_timer µs. The log time in brackets has another origin (CPU cycle counter at the first log call
     * + FreeRTOS ticks) and ran ~0.54 s ahead of it on 2026-10-29: this line prints both to correlate samples and logs */
    ESP_LOGI(TAG, "recording %s started (%s) tag '%s', %" PRIu32 " blocks free, sample clock %lld ms", r->rec_id, by ? by : "?", r->rec_tag,
             free_blocks(), (long long)(esp_timer_get_time() / 1000));
    xSemaphoreGive(s_ctl);
    if (s_hook) s_hook(true, r, by);
    return ESP_OK;
}

esp_err_t store_stop(const char *by, store_rec_t *out)
{
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (s_state != REC_RECORDING) { xSemaphoreGive(s_ctl); return ESP_ERR_INVALID_STATE; }
    s_state = REC_STOPPING;
    s_feed_on = false;
    vTaskDelay(pdMS_TO_TICKS(5));                     /* any in-flight store_feed() on core 1 has returned */
    int64_t stop_mono = esp_timer_get_time(), stop_utc = clock_utc_ms();

    for (int i = 0; i < 200 && s_wr - s_rd >= RING_BLOCKS - 2; i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_fill) block_close();                        /* partial last block */
    /* END2 record, handed to the writer like a block so the file is closed in stream order */
    raw_blk_t *rb = ring_blk(s_wr);
    char *j = (char *)rb->data + 8;
    int n = snprintf(j, sizeof rb->data - 8,
        "{\"stop_anchor\":{\"mono_us\":%lld,\"utc_ms\":%lld,\"synced\":%s},\"samples\":%" PRIu32 ",\"blocks\":%" PRIu32
        ",\"dropped\":%" PRIu32 ",\"saturated\":%" PRIu32 ",\"gaps\":%" PRIu32 ",\"gap_max_us\":%" PRIu32 ",\"stopped_by\":\"%s\",\"complete\":true,"
        "\"first_sample_mono_us\":%lld,\"last_sample_mono_us\":%lld,\"temp_c_stop\":%.1f}",
        (long long)stop_mono, (long long)stop_utc, stop_utc ? "true" : "false", s_samples, s_blocks + (s_wr - s_rd), s_dropped, s_saturated, s_gaps, s_gap_max_us,
        by ? by : "", (long long)s_first_mono, (long long)s_last_mono, board_temp_c());
    memcpy(rb->data, "END2", 4); uint32_t jl = n; memcpy(rb->data + 4, &jl, 4);
    rb->n = 8 + n; rb->kind = 1; rb->flags = 0;
    raw_commit();
    for (int i = 0; i < 600 && !(s_rd == s_wr && s_sha_done); i++) vTaskDelay(pdMS_TO_TICKS(5));   /* ≤ 3 s drain */
    if (s_rd != s_wr) ESP_LOGE(TAG, "writer did not drain (%" PRIu32 " blocks left)", s_wr - s_rd);
    if (s_lost_pages) ESP_LOGE(TAG, "%" PRIu32 " pages lost to bad blocks / full storage", s_lost_pages);

    store_rec_t *r = &s_rec[s_cur];
    fill_live(r);
    r->pages = s_pg_idx; r->bytes = s_pg_off; r->stop_utc_ms = stop_utc;
    r->duration_ms = (uint32_t)((stop_mono - r->start_mono_us) / 1000);
    r->complete = (s_rd == s_wr) && s_sha_done && s_lost_pages == 0;
    r->sha_known = s_sha_done;
    mbedtls_sha256_free(&s_sha);
    if (s_wblk_next >= 0) { s_map[s_wblk_next] = MAP_FREE; s_wblk_next = -1; }
    config_set_locked(false);
    clock_resync();
    s_state = REC_IDLE;
    ESP_LOGI(TAG, "recording %s stopped (%s): %" PRIu32 " samples in %" PRIu32 " blocks, %" PRIu32 " bytes, %" PRIu32 " dropped, %" PRIu32 " saturated, backlog max %" PRIu32 ", %" PRIu32 " blocks free",
             r->rec_id, by ? by : "?", r->samples, r->blocks, r->bytes, r->dropped, r->saturated, s_backlog_max, free_blocks());
    if (s_gaps) ESP_LOGW(TAG, "recording %s: %" PRIu32 " sampling gap(s), longest %" PRIu32 " us (> %" PRIu32 " us = 1.5 periods)",
                         r->rec_id, s_gaps, s_gap_max_us, s_gap_thr_us);
    else        ESP_LOGI(TAG, "recording %s: no sampling gaps (threshold %" PRIu32 " us)", r->rec_id, s_gap_thr_us);
    if (out) *out = *r;
    xSemaphoreGive(s_ctl);
    if (s_hook) s_hook(false, r, by);
    return ESP_OK;
}

bool store_is_recording(void) { return s_state != REC_IDLE; }

void store_get_status(store_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->state = s_state; st->nand_ok = s_nand_ok;
    if (s_state != REC_IDLE) {
        st->cur = s_rec[s_cur]; fill_live(&st->cur);
        st->elapsed_ms = (uint32_t)((esp_timer_get_time() - st->cur.start_mono_us) / 1000);
    }
    uint32_t total = 0;
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) if (s_map[b] != MAP_BAD) total++;
    st->total_mb = (total * NAND_BLOCK_SIZE) >> 20;
    st->free_mb  = (free_blocks() * NAND_BLOCK_SIZE) >> 20;
    for (int i = 0; i < MAX_REC; i++) if (s_rec[i].used && !(s_state != REC_IDLE && i == s_cur)) st->count++;
    st->backlog_max = s_backlog_max; st->storage_full = s_full;
}

int store_count(void) { return MAX_REC; }
const store_rec_t *store_rec(int i) { return (i >= 0 && i < MAX_REC && s_rec[i].used && !(s_state != REC_IDLE && i == s_cur)) ? &s_rec[i] : NULL; }
const store_rec_t *store_find(const char *rec_id)
{
    for (int i = 0; i < MAX_REC; i++) if (s_rec[i].used && strcmp(s_rec[i].rec_id, rec_id) == 0) return &s_rec[i];
    return NULL;
}
void store_set_uploaded(const char *rec_id, uint32_t bytes)
{
    store_rec_t *r = (store_rec_t *)store_find(rec_id);
    if (r) r->uploaded_bytes = bytes;
}

esp_err_t store_delete(const char *rec_id)
{
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (s_state != REC_IDLE) { xSemaphoreGive(s_ctl); return ESP_ERR_INVALID_STATE; }
    const store_rec_t *r = store_find(rec_id);
    if (!r) { xSemaphoreGive(s_ctl); return ESP_ERR_NOT_FOUND; }
    int slot = r - s_rec, n = 0;
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) {
        if (s_map[b] != slot) continue;
        if (nand_block_erase(b) == ESP_OK) s_map[b] = MAP_FREE; else { nand_mark_bad(b); s_map[b] = MAP_BAD; }
        n++;
    }
    s_rec[slot].used = false;
    ESP_LOGI(TAG, "deleted %s (%d blocks)", rec_id, n);
    xSemaphoreGive(s_ctl);
    return ESP_OK;
}

int store_read(const char *rec_id, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (s_state != REC_IDLE) return -ESP_ERR_INVALID_STATE;
    const store_rec_t *r = store_find(rec_id);
    if (!r) return -ESP_ERR_NOT_FOUND;
    if (offset >= r->bytes) return 0;
    if (offset + len > r->bytes) len = r->bytes - offset;
    int slot = r - s_rec;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    if (s_state != REC_IDLE) { xSemaphoreGive(s_ctl); return -ESP_ERR_INVALID_STATE; }   /* a recording started meanwhile */
    static uint8_t *pg;                               /* PSRAM: internal RAM is scarce with two TLS sessions */
    if (!pg) pg = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pg) { xSemaphoreGive(s_ctl); return -ESP_ERR_NO_MEM; }
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = offset + done, idx = pos / STORE_PAYLOAD, in = pos % STORE_PAYLOAD;   /* all pages full but the last */
        uint32_t n = STORE_PAYLOAD - in; if (n > len - done) n = len - done;
        if (read_page(slot, idx, pg) != ESP_OK) memset(pg + STORE_HDR_BYTES, 0, STORE_PAYLOAD);   /* lost block: zeros */
        memcpy(buf + done, pg + STORE_HDR_BYTES + in, n);
        done += n;
    }
    xSemaphoreGive(s_ctl);
    return (int)done;
}

esp_err_t store_sha256(const char *rec_id, uint8_t out[32])
{
    if (s_state != REC_IDLE) return ESP_ERR_INVALID_STATE;
    store_rec_t *r = (store_rec_t *)store_find(rec_id);
    if (!r) return ESP_ERR_NOT_FOUND;
    if (!r->sha_known) {                               /* incomplete recording (or digest lost): hash it now */
        mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
        uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) { mbedtls_sha256_free(&c); return ESP_ERR_NO_MEM; }
        int64_t t0 = esp_timer_get_time();
        for (uint32_t off = 0; off < r->bytes;) {
            int n = store_read(rec_id, off, buf, 4096);
            if (n <= 0) { mbedtls_sha256_free(&c); free(buf); return n < 0 ? (esp_err_t)-n : ESP_FAIL; }
            mbedtls_sha256_update(&c, buf, n); off += n;
        }
        mbedtls_sha256_finish(&c, r->sha256); mbedtls_sha256_free(&c); free(buf);
        r->sha_known = true;
        ESP_LOGI(TAG, "%s: SHA-256 computed over %" PRIu32 " bytes in %lld ms", rec_id, r->bytes, (long long)((esp_timer_get_time() - t0) / 1000));
    }
    memcpy(out, r->sha256, 32);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* boot: rebuild the inventory from the page headers                    */
/* ------------------------------------------------------------------ */
static int last_written_page(uint16_t blk, store_page_hdr_t *out)   /* pages are written in order: binary search */
{
    int lo = 0, hi = NAND_PAGES_PER_BLK - 1, best = -1;
    store_page_hdr_t h;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (nand_page_read((uint32_t)blk * NAND_PAGES_PER_BLK + mid, 0, (uint8_t *)&h, sizeof h, NULL) == ESP_OK && hdr_valid(&h)) { best = mid; *out = h; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

static void scan(void)
{
    int64_t t0 = esp_timer_get_time();
    memset(s_map, MAP_FREE, sizeof s_map);
    for (int b = 0; b < NAND_BLOCKS; b++) if (b < STORE_FIRST_BLK || b > STORE_LAST_BLK) s_map[b] = MAP_BAD;   /* reserved */
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) {
        if (nand_block_is_bad(b)) { s_map[b] = MAP_BAD; continue; }
        store_page_hdr_t h;
        if (nand_page_read((uint32_t)b * NAND_PAGES_PER_BLK, 0, (uint8_t *)&h, sizeof h, NULL) != ESP_OK) continue;
        if (!hdr_valid(&h)) continue;                 /* blank, old format or garbage: erased before reuse */
        int slot = slot_find(h.boot_id, h.rec_n);
        if (slot < 0) {
            slot = slot_alloc();
            if (slot < 0) { ESP_LOGW(TAG, "more than %d recordings on the NAND; block %d ignored", MAX_REC, b); continue; }
            s_rec[slot].boot_id = h.boot_id; s_rec[slot].rec_n = h.rec_n;
            make_id(s_rec[slot].rec_id, h.boot_id, h.rec_n);
        }
        s_map[b] = slot; s_ord[b] = h.page / NAND_PAGES_PER_BLK;
    }
    uint8_t *pg = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *tail = heap_caps_malloc(2 * STORE_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pg || !tail) { free(pg); free(tail); return; }
    for (int i = 0; i < MAX_REC; i++) {
        store_rec_t *r = &s_rec[i];
        if (!r->used) continue;
        /* file header: tag, anchors, rate */
        if (read_page(i, 0, pg) == ESP_OK && STORE_PG_TYPE(((store_page_hdr_t *)pg)->type) == STORE_PG_FILEHDR) {
            uint32_t jl; memcpy(&jl, pg + STORE_HDR_BYTES + 8, 4);
            if (jl > STORE_PAYLOAD - 12) jl = STORE_PAYLOAD - 12;
            cJSON *j = cJSON_ParseWithLength((char *)pg + STORE_HDR_BYTES + 12, jl);
            if (j) {
                const cJSON *t = cJSON_GetObjectItem(j, "rec_tag"); if (cJSON_IsString(t)) strncpy(r->rec_tag, t->valuestring, sizeof r->rec_tag - 1);
                const cJSON *rate = cJSON_GetObjectItem(j, "nominal_rate_hz"); if (cJSON_IsNumber(rate)) r->odr_hz = (int)rate->valuedouble;
                const cJSON *a = cJSON_GetObjectItem(j, "start_anchor");
                const cJSON *mu = a ? cJSON_GetObjectItem(a, "mono_us") : NULL, *uu = a ? cJSON_GetObjectItem(a, "utc_ms") : NULL;
                if (cJSON_IsNumber(mu)) r->start_mono_us = (int64_t)mu->valuedouble;
                if (cJSON_IsNumber(uu)) r->start_utc_ms = (int64_t)uu->valuedouble;
                cJSON_Delete(j);
            }
        }
        /* last written page */
        uint16_t maxord = 0; int lastb = -1;
        for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) if (s_map[b] == i && (lastb < 0 || s_ord[b] >= maxord)) { maxord = s_ord[b]; lastb = b; }
        store_page_hdr_t lh; int lp = lastb >= 0 ? last_written_page(lastb, &lh) : -1;
        if (lp < 0) { r->used = false; continue; }
        if (nand_page_read((uint32_t)lastb * NAND_PAGES_PER_BLK + lp, 0, pg, NAND_PAGE_SIZE, NULL) != ESP_OK) { r->used = false; continue; }
        if (STORE_PG_TYPE(lh.type) == STORE_PG_END) {
            /* complete file. The digest sits in this page; the stream may end here or (len 0) in the page before. */
            if (lh.type & STORE_PG_HAS_SHA) { memcpy(r->sha256, pg + STORE_HDR_BYTES + STORE_SHA_OFF, 32); r->sha_known = true; }
            uint32_t end;
            if (lh.len == 0) { store_page_hdr_t ph; if (read_hdr(i, lh.page - 1, &ph) != ESP_OK) { r->used = false; continue; } end = ph.offset + ph.len; r->pages = lh.page + 1; }
            else { end = lh.offset + lh.len; r->pages = lh.page + 1; }
            r->bytes = end; r->complete = true;
            /* END2 record = the last 8+json_len bytes; find its magic in the last two slices */
            uint32_t from = end > 2 * STORE_PAYLOAD ? end - 2 * STORE_PAYLOAD : 0, got = end - from;
            int rd = store_read(r->rec_id, from, tail, got);
            if (rd == (int)got) {
                for (int k = (int)got - 8; k >= 0; k--) {
                    uint32_t jl; memcpy(&jl, tail + k + 4, 4);
                    if (memcmp(tail + k, "END2", 4) == 0 && k + 8 + jl == got) {
                        cJSON *j = cJSON_ParseWithLength((char *)tail + k + 8, jl);
                        if (j) {
                            const cJSON *v;
                            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "samples")))   r->samples = (uint32_t)v->valuedouble;
                            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "blocks")))    r->blocks = (uint32_t)v->valuedouble;
                            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "dropped")))   r->dropped = (uint32_t)v->valuedouble;
                            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "saturated"))) r->saturated = (uint32_t)v->valuedouble;
                            const cJSON *a = cJSON_GetObjectItem(j, "stop_anchor");
                            const cJSON *mu = a ? cJSON_GetObjectItem(a, "mono_us") : NULL, *uu = a ? cJSON_GetObjectItem(a, "utc_ms") : NULL;
                            if (cJSON_IsNumber(uu)) r->stop_utc_ms = (int64_t)uu->valuedouble;
                            if (cJSON_IsNumber(mu)) r->duration_ms = (uint32_t)(((int64_t)mu->valuedouble - r->start_mono_us) / 1000);
                            cJSON_Delete(j);
                        }
                        break;
                    }
                }
            }
        } else {
            /* power lost while recording: walk the blocks; the file ends after the last complete one */
            r->pages = lh.page + 1; r->bytes = lh.offset + lh.len; r->complete = false;
            uint32_t off = STORE_PAYLOAD, samples = 0, blocks = 0; pcr2_block_hdr_t bh;
            while (off + PCR2_BLOCK_HDR <= r->bytes && store_read(r->rec_id, off, (uint8_t *)&bh, PCR2_BLOCK_HDR) == PCR2_BLOCK_HDR) {
                if (memcmp(bh.magic, "BLK2", 4) != 0 || off + PCR2_BLOCK_HDR + bh.payload_len > r->bytes) break;
                off += PCR2_BLOCK_HDR + bh.payload_len; samples = bh.first_sample_index + bh.n_samples; blocks++;
            }
            r->bytes = off; r->samples = samples; r->blocks = blocks;
            r->duration_ms = r->odr_hz ? (uint32_t)((uint64_t)samples * 1000 / r->odr_hz) : 0;
        }
    }
    free(tail);
    free(pg);
    int n = 0; for (int i = 0; i < MAX_REC; i++) if (s_rec[i].used) n++;
    ESP_LOGI(TAG, "inventory: %d recordings, %" PRIu32 " of %d blocks free (%" PRIu32 " MB), scan %lld ms",
             n, free_blocks(), STORE_LAST_BLK - STORE_FIRST_BLK + 1, (free_blocks() * NAND_BLOCK_SIZE) >> 20, (long long)((esp_timer_get_time() - t0) / 1000));
    for (int i = 0; i < MAX_REC; i++) if (s_rec[i].used)
        ESP_LOGI(TAG, "  %-28s tag '%s' %" PRIu32 " samples %" PRIu32 " bytes %s%s", s_rec[i].rec_id, s_rec[i].rec_tag, s_rec[i].samples, s_rec[i].bytes,
                 s_rec[i].complete ? "complete" : "INCOMPLETE", s_rec[i].sha_known ? ", sha cached" : "");
}

/* ------------------------------------------------------------------ */
/* synthetic recording (test aid)                                      */
/* ------------------------------------------------------------------ */
static TaskHandle_t s_gen;
static uint32_t     s_gen_samples;

/* a plausible padel signal without transcendental calls per sample: oscillators as incremental rotations
 * (x' = x cos w - y sin w), gravity on z, an impact burst on the high-g channel every ~3 s from a small table */
typedef struct { float c, s, x, y; } osc_t;
static void osc_init(osc_t *o, float hz) { float w = 2.0f * (float)M_PI * hz / 927.0f; o->c = cosf(w); o->s = sinf(w); o->x = 1; o->y = 0; }
static inline float osc_step(osc_t *o) { float x = o->x * o->c - o->y * o->s; o->y = o->x * o->s + o->y * o->c; o->x = x; return o->y; }
static osc_t s_osc[4]; static int16_t s_impact[20];

static void synth_init(void)
{
    osc_init(&s_osc[0], 1.0f / 2.7f); osc_init(&s_osc[1], 2.3f / (2.0f * (float)M_PI)); osc_init(&s_osc[2], 1.1f / (2.0f * (float)M_PI)); osc_init(&s_osc[3], 0.7f / (2.0f * (float)M_PI));
    for (int k = 0; k < 20; k++) s_impact[k] = (int16_t)(20000 * expf(-k / 6.0f) * (k & 1 ? 1 : -1));
}

static inline void synth_sample(imu_sample_t *o, uint32_t i, uint32_t *seed)
{
    /* noise at the real sensor's level (±6 / ±3 / ±2 LSB): a 150-min file then compresses to ~40-45 MB like a real match */
    *seed = *seed * 1664525u + 1013904223u; int n1 = (int)((*seed >> 16) % 13) - 6;
    *seed = *seed * 1664525u + 1013904223u; int n2 = (int)((*seed >> 16) % 7) - 3;
    *seed = *seed * 1664525u + 1013904223u; int n3 = (int)((*seed >> 16) % 5) - 2;
    float swing = osc_step(&s_osc[0]), a = osc_step(&s_osc[1]), b = osc_step(&s_osc[2]), c = osc_step(&s_osc[3]);
    o->v[0] = (int16_t)(12 * 256);
    o->v[1] = (int16_t)(3000 * swing + n1);
    o->v[2] = (int16_t)(1500 * a + n1);
    o->v[3] = (int16_t)(800 * b + n1);
    o->v[4] = (int16_t)(1200 * swing + n2);
    o->v[5] = (int16_t)(600 * c + n2);
    o->v[6] = (int16_t)(2049 + 900 * swing * swing + n2);                   /* 1 g on z */
    uint32_t k = i % 2781;                                                   /* impact every 3 s, 20 samples wide */
    int16_t imp = k < 20 ? s_impact[k] : 0;
    o->v[7] = (int16_t)(imp + n3); o->v[8] = (int16_t)(imp / 3 + n3); o->v[9] = (int16_t)(100 + n3);
}

static void gen_task(void *arg)
{
    (void)arg;
    uint32_t n = s_gen_samples, seed = 0x5EED;
    int64_t t = esp_timer_get_time(), t0 = t;
    imu_sample_t smp;
    synth_init();
    ESP_LOGW(TAG, "generating %" PRIu32 " synthetic samples (%" PRIu32 " min)", n, n / 927 / 60);
    for (uint32_t i = 0; i < n && s_state == REC_RECORDING; i++) {
        while (s_wr - __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE) >= RING_BLOCKS - 1) vTaskDelay(1);   /* let the writer catch up */
        synth_sample(&smp, i, &seed);
        t += 1078 + (int)((seed >> 20) % 7) - 3;                            /* 927 Hz with ±3 µs jitter */
        feed_impl(&smp, t);
        /* no explicit yield needed: the task runs at idle priority, so IDLE1 (which feeds the task watchdog) is time-sliced in */
    }
    ESP_LOGW(TAG, "generation done in %lld s", (long long)((esp_timer_get_time() - t0) / 1000000));
    s_synth = false;
    store_stop("synthetic", NULL);
    s_gen = NULL;
    vTaskDelete(NULL);
}

esp_err_t store_generate(uint32_t minutes, const char *rec_tag)
{
    if (s_gen || s_state != REC_IDLE) return ESP_ERR_INVALID_STATE;
    if (minutes < 1) minutes = 1;
    if (minutes > 600) minutes = 600;
    s_gen_samples = minutes * 60 * 927;
    s_synth = true;
    esp_err_t e = store_start(rec_tag && *rec_tag ? rec_tag : "synthetic", "synthetic", NULL);
    if (e != ESP_OK) { s_synth = false; return e; }
    if (xTaskCreatePinnedToCore(gen_task, "synth", 4096, NULL, tskIDLE_PRIORITY, &s_gen, 1) != pdPASS)   /* idle prio: see gen_task */ { s_synth = false; store_stop("synthetic", NULL); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t store_init(const char *device_id, uint32_t boot_id, bool nand_ok)
{
    strncpy(s_devid, device_id, sizeof s_devid - 1);
    s_boot_id = boot_id; s_nand_ok = nand_ok;
    s_ctl = xSemaphoreCreateMutex();
    memset(s_map, MAP_BAD, sizeof s_map);
    if (esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) != 0xCBF43926) ESP_LOGE(TAG, "CRC-32 self-test FAILED");   /* data-v2 §1.2 */
    if (!nand_ok) { ESP_LOGE(TAG, "NAND not available: recording disabled"); return ESP_FAIL; }
    s_ring = heap_caps_malloc(RING_BLOCKS * sizeof(raw_blk_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pg   = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_enc  = heap_caps_malloc(PCR2_BLOCK_HDR + PCR2_SPB * PCR2_SAMPLE_BYTES + 2 * PCR2_FIELDS * ((PCR2_SPB + 31) / 32), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || !s_pg || !s_enc) { ESP_LOGE(TAG, "no PSRAM for the recording buffers"); s_nand_ok = false; return ESP_ERR_NO_MEM; }
    scan();
    int last = STORE_FIRST_BLK - 1;
    for (int b = STORE_FIRST_BLK; b <= STORE_LAST_BLK; b++) if (s_map[b] < MAX_REC) last = b;
    s_alloc_cursor = (last >= STORE_LAST_BLK) ? STORE_FIRST_BLK : last + 1;
    task_create_psram(writer_task, "nand_writer", 6144, NULL, 10, &s_writer, 1);   /* SPI NAND + SHA only: no flash-cache-disabling calls */
    return ESP_OK;
}

void store_set_event_hook(store_event_fn fn) { s_hook = fn; }
const char *store_state_name(store_state_t s) { static const char *n[] = { "idle", "recording", "stopping" }; return n[s <= REC_STOPPING ? s : 0]; }
