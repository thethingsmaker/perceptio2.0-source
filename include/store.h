/*
 * Recording store — PCR2 files (docs/data-v2.md) written directly on the W25N01KV NAND. No filesystem.
 *
 * The file is a byte stream: file header, codec-1 blocks (400 samples each, ~2.2 KB encoded), END2 record. On the
 * NAND it is cut into 2032 B slices: every 2048 B page = 16 B container header (store_page_hdr_t: which recording,
 * page index, stream bytes, stream offset) + up to 2032 stream bytes. All pages are full except the last, so page
 * index = offset / 2032. The upload is the concatenation of the stream bytes — never re-encoded.
 * A power cut loses the block being filled plus the page being assembled (≤ ~1 s); the file stays valid (no END2 =
 * incomplete, decoded up to the last complete block).
 * The file's SHA-256 is written into the END page outside the stream (or into one extra page when it does not fit).
 *
 * Layout on the NAND: blocks 8..1019 hold recordings; 0..7 and 1020..1023 are reserved (self-test scratch).
 * Inventory = boot-time scan of page 0 of every block (~0.4 s); incomplete files are walked block by block.
 *
 * Threads: store_feed() in the core-1 sampler task fills raw 400-sample blocks in a PSRAM ring (lock-free, never
 * blocks); a core-1 writer task encodes each block (codec 1), appends it to the page assembler, programs full pages
 * and hashes the stream as it goes. Control calls come from core 0. While recording nothing else touches the NAND
 * or the internal flash.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "imu.h"
#include "w25n01kv.h"

/* ---- NAND page container ---- */
#define STORE_MAGIC      0x3450                      /* "P4": PCR2 stream slices (0x3350 = one block per page, 0x3250 = pre-PCR2) */
#define STORE_PG_FILEHDR 1
#define STORE_PG_BLOCK   2
#define STORE_PG_END     3
#define STORE_HDR_BYTES  16
#define STORE_PAYLOAD    (NAND_PAGE_SIZE - STORE_HDR_BYTES)          /* 2032 stream bytes per page */
#define STORE_FIRST_BLK  8
#define STORE_LAST_BLK   1019

typedef struct __attribute__((packed)) {
    uint16_t magic;        /* STORE_MAGIC */
    uint8_t  type;         /* STORE_PG_* in bits 0..3; bit 7: the END page also carries the SHA-256 at payload[2000..2031] */
    uint8_t  rec_n;        /* recording identity = (boot_id, rec_n) */
    uint32_t boot_id;
    uint16_t page;         /* index within the recording (0 = file header) */
    uint16_t len;          /* stream bytes in this page (≤ STORE_PAYLOAD; partial blocks are shorter) */
    uint32_t offset;       /* stream offset of this page's first byte (pages have variable length) */
} store_page_hdr_t;
#define STORE_PG_TYPE(t)  ((t) & 0x0F)
#define STORE_PG_HAS_SHA  0x80
#define STORE_SHA_OFF     2000                       /* in the END page payload, outside the stream */

/* ---- PCR2 (data-v2.md §1), little-endian ---- */
#define PCR2_SAMPLE_BYTES   20                       /* dt_us u16 + gx gy gz ax ay az hx hy hz i16 */
#define PCR2_FIELDS         10
#define PCR2_BLOCK_HDR      32
#define PCR2_SPB            400                      /* samples per block (backend recommendation for codec 1) */
#define PCR2_CODEC          1                        /* delta bit-packing, §1.4 */
#define PCR2_MAX_DT_US      65535

typedef struct __attribute__((packed)) {
    char     magic[4];     /* "BLK2" */
    uint32_t block_seq;
    uint64_t first_mono_us;
    uint32_t first_sample_index;
    uint16_t n_samples;
    uint8_t  codec;        /* 0 raw, 1 delta bit-packed */
    uint8_t  flags;        /* bit0: samples lost just before this block */
    uint32_t payload_len;
    uint32_t crc32;        /* IEEE, esp_rom_crc32_le(0, payload, payload_len) */
} pcr2_block_hdr_t;

/* ---- runtime ---- */
typedef enum { REC_IDLE = 0, REC_RECORDING, REC_STOPPING } store_state_t;

typedef struct {
    bool     used;
    char     rec_id[32];                     /* "<device_id>-<boot_id>-<rec_n>" */
    char     rec_tag[65];
    uint32_t boot_id; uint8_t rec_n;
    int64_t  start_mono_us, start_utc_ms, stop_utc_ms;
    uint32_t pages;                          /* NAND pages incl. header/end */
    uint32_t bytes;                          /* PCR2 file size */
    uint32_t samples, blocks, dropped, saturated;
    uint32_t duration_ms;
    int      odr_hz;
    bool     complete;                       /* END2 present */
    bool     this_boot;
    bool     sha_known;
    uint8_t  sha256[32];
    uint32_t uploaded_bytes;                 /* progress reported by the upload task (RAM only) */
} store_rec_t;

typedef struct {
    store_state_t state;
    store_rec_t   cur;                       /* while recording: live counters */
    uint32_t      elapsed_ms;
    uint32_t      total_mb, free_mb;
    int           count;                     /* stored recordings */
    uint32_t      backlog_max;               /* writer ring high-water mark (pages) */
    bool          storage_full;              /* writer ran out of blocks: the session must be stopped */
    bool          nand_ok;
} store_status_t;

typedef void (*store_event_fn)(bool started, const store_rec_t *rec, const char *by);

esp_err_t   store_init(const char *device_id, uint32_t boot_id, bool nand_ok);   /* boot, core 0: builds the inventory */
void        store_set_event_hook(store_event_fn fn);
/* ESP_ERR_INVALID_STATE already recording, ESP_ERR_NO_MEM storage/inventory full, ESP_FAIL NAND absent. */
esp_err_t   store_start(const char *rec_tag, const char *by, char rec_id_out[32]);
esp_err_t   store_stop(const char *by, store_rec_t *out);     /* ESP_ERR_INVALID_STATE when idle; blocks ≤ 2 s for the flush */
bool        store_is_recording(void);
void        store_feed(const imu_sample_t *s, int64_t t_us); /* sampler sink, core 1 */
void        store_get_status(store_status_t *st);
int         store_count(void);
const store_rec_t *store_rec(int i);                          /* inventory slot i (NULL if unused or the running one) */
const store_rec_t *store_find(const char *rec_id);
esp_err_t   store_delete(const char *rec_id);                 /* erases its blocks; ESP_ERR_INVALID_STATE while recording */
/* Read `len` bytes of the PCR2 stream from `offset`. Returns bytes read (0 at EOF), <0 = -esp_err
 * (ESP_ERR_INVALID_STATE while recording). A lost block reads as zeros. */
int         store_read(const char *rec_id, uint32_t offset, uint8_t *buf, uint32_t len);
/* SHA-256 of the whole file: cached when written this boot or read back from the END page; otherwise computed by
 * reading the file (≈ 1 s per 2 MB). ESP_ERR_INVALID_STATE while recording. */
esp_err_t   store_sha256(const char *rec_id, uint8_t out[32]);
void        store_set_uploaded(const char *rec_id, uint32_t bytes);
const char *store_state_name(store_state_t s);
/* Test aid: write a synthetic recording of `minutes` at the nominal rate as fast as the NAND allows (~1 min per 150 min
 * of data), through the normal pipeline (codec, hashing, events, upload). The live IMU feed is ignored meanwhile.
 * ESP_ERR_INVALID_STATE if a recording/generation is running. */
esp_err_t   store_generate(uint32_t minutes, const char *rec_tag);
