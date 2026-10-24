# Perceptio 2.0 — recording format and upload protocol

Status: **draft 1, 2026-10-22**. Companion to `protocol-v2.md` (control channel). This document covers how a
recording is laid out on the racket's NAND, how it travels to the backend, and what the backend guarantees.

Design rules:
1. **The racket uploads the file exactly as it wrote it.** No re-encoding on the device: the upload is a byte copy.
2. **The file describes itself.** The header carries sensor ranges, scale factors and the byte layout of a sample,
   so the backend decodes any layout the firmware chooses, and old files stay readable forever.
3. **Every block is checksummed.** A damaged block is detected and skipped, not silently decoded.
4. **Every sample carries time.** Per-sample timestamps (µs) so gaps, jitter and the real rate are measurable.
   The racket's design rate is **927 Hz** (confirmed by the user 2026-10-22); write it as `nominal_rate_hz`.
5. **Nothing is deleted on the racket until the backend has verified the whole file.**

---

## 1. Recording file — `PCR2`

Little-endian throughout. One file per recording, named by `rec_id`.

```
+---------------------------+
| File header               |  magic "PCR2", version, JSON metadata
+---------------------------+
| Block 0                   |  block header (32 B) + payload (samples)
| Block 1                   |
| ...                       |
+---------------------------+
| End record                |  magic "END2" + JSON (stop anchor, totals)   — written at stop
+---------------------------+
```

### 1.1 File header

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | `"PCR2"` |
| 4 | u16 | `format_version` = 1 |
| 6 | u16 | reserved = 0 |
| 8 | u32 | `json_len` |
| 12 | u8[json_len] | UTF-8 JSON (below), no terminator |
| … | u8[pad] | zero padding to a multiple of 4 bytes |

```json
{
  "rec_id": "d40592485f78-17-0", "device_id": "d40592485f78", "boot_id": 17, "rec_tag": "m-24/p-51",
  "fw": "3.0.0", "hw_rev": "R2", "started_by": "ui",
  "start_anchor": { "mono_us": 4210331000, "utc_ms": 1792694001191, "synced": true },
  "nominal_rate_hz": 927,
  "sample": {
    "size": 20,
    "fields": [
      { "name": "dt_us", "type": "u16", "unit": "us" },
      { "name": "gx",  "type": "i16", "scale": 0.0610352, "unit": "dps" },
      { "name": "gy",  "type": "i16", "scale": 0.0610352, "unit": "dps" },
      { "name": "gz",  "type": "i16", "scale": 0.0610352, "unit": "dps" },
      { "name": "a1x", "type": "i16", "scale": 0.000488281, "unit": "g", "sensor": "accel_16g" },
      { "name": "a1y", "type": "i16", "scale": 0.000488281, "unit": "g", "sensor": "accel_16g" },
      { "name": "a1z", "type": "i16", "scale": 0.000488281, "unit": "g", "sensor": "accel_16g" },
      { "name": "a2x", "type": "i16", "scale": 0.009765625, "unit": "g", "sensor": "accel_320g" },
      { "name": "a2y", "type": "i16", "scale": 0.009765625, "unit": "g", "sensor": "accel_320g" },
      { "name": "a2z", "type": "i16", "scale": 0.009765625, "unit": "g", "sensor": "accel_320g" }
    ]
  },
  "sensors": {
    "gyro":       { "part": "TBD", "fsr_dps": 2000, "odr_hz": 1000 },
    "accel_16g":  { "part": "TBD", "fsr_g": 16,  "odr_hz": 1000 },
    "accel_320g": { "part": "TBD", "fsr_g": 320, "odr_hz": 1000 }
  },
  "axes": "racket frame: x = along the handle towards the head, y = across the face, z = out of the face"
}
```

- `sample.fields` **is the contract**: the backend decodes exactly what it says, in order. Types: `u8 i8 u16 i16
  u32 i32 f32`. `value_in_unit = raw × scale` (scale defaults to 1). The scales above are examples for
  ±2000 dps / ±16 g / ±320 g on 16 bits — **the firmware writes its real ones**.
- Field `dt_us` (required, `u16`): µs since the previous sample (0 for the block's first sample, whose time is in the
  block header). A gap > 65.535 ms starts a new block instead.
- `size` must equal the sum of the field sizes.
- Unknown JSON keys are kept and shown, never refused.

### 1.2 Block

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | `"BLK2"` |
| 4 | u32 | `block_seq` (0, 1, 2 … no gaps) |
| 8 | u64 | `first_mono_us` — racket monotonic µs of the block's first sample |
| 16 | u32 | `first_sample_index` (index of its first sample within the recording) |
| 20 | u16 | `n_samples` |
| 22 | u8 | `codec` — 0 = raw samples, **1 = delta bit-packed (§1.4, recommended)** |
| 23 | u8 | `flags` — bit0 = samples were lost just before this block (the firmware knows) |
| 24 | u32 | `payload_len` (bytes; = n_samples × sample.size when codec 0; the encoded length when codec 1) |
| 28 | u32 | `crc32` of the payload **as stored** (encoded bytes for codec 1; IEEE 802.3, as `esp_crc32_le(0, …)`) |
| 32 | u8[payload_len] | samples |

Recommended block size: **one NAND page multiple, ~4–16 KB of payload** (200–800 samples). A block is written whole
or not at all; a power cut loses at most the block being filled.

### 1.4 Codec 1 — delta bit-packing (added 2026-10-22)

Lossless. Measured on Padel 0's real 40-minute recording `d40592485f78-24-0`: **46.5 MB → 12.8 MB (3.6×)**, the same
as zlib on the same data, without a compression library. It also stretches the NAND from ~80 to ~290 minutes.
Codecs 0 and 1 may be mixed in one file; the backend decodes both (live since 2026-10-22).

Payload for a block of `n` samples (`n = 0` → empty payload). For **each field, in header order** (integer types only;
an `f32` field needs codec 0):
1. the block's first value of that field, in the field's own size, little-endian (exactly as in a raw sample);
2. the `n − 1` differences to the previous sample, in **frames of 32** (the last one may be shorter). Each frame:
   - `u8 w` = bit width of the frame's largest encoded value (0 when the field did not change);
   - the frame's values, each **zigzag(delta)** packed at `w` bits, **LSB first**, then zero bits to the next byte.

`delta = (cur − prev)` computed modulo 2^bits of the field (it wraps, so `w ≤ 8 × size`); `zigzag(d) = (d << 1) ^
(d >> (bits−1))` on the signed delta. Reference encoders, byte-for-byte identical (400 random layouts tested):
`pcr2_codec1.c` (for the firmware) and `make_pcr2.encode_c1` (Python). Decoder: `pcr2.py`.

Encode **when the block is written** (writer task, never the sampling path), so the upload is still a byte copy of the
file. CRC and SHA-256 are over the encoded bytes.

### 1.3 End record (written at stop)

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | `"END2"` |
| 4 | u32 | `json_len` |
| 8 | u8[json_len] | JSON |

```json
{ "stop_anchor": { "mono_us": 4430873000, "utc_ms": 1792694221733, "synced": true },
  "samples": 204452, "blocks": 512, "dropped": 0, "stopped_by": "ui", "complete": true }
```

A file without `END2` (racket died mid-recording) is still valid: every whole block is decoded, and the recording
is marked **incomplete**.

---

## 2. Upload — resumable HTTPS, scheduled by the backend

Endpoint host: **`https://ingest.padelsense.net`** (Cloudflare-proxied, TLS verified with the certificate bundle).

### 2.1 The backend asks (MQTT command, see protocol-v2 §8)

```json
{ "id": "c-…", "cmd": "upload", "expires_utc_ms": …,
  "args": { "rec_id": "d40592485f78-17-0",
            "url": "https://ingest.padelsense.net/v1/uploads/u-4f2a9c",
            "token": "<one-time, 64 hex>", "chunk_bytes": 524288 } }
```

- The backend issues `upload` when a racket is **online and not recording**, one recording at a time per racket and
  a few rackets at a time overall (venue Wi-Fi). The dashboard can also request one ("Upload now").
- The token is valid for **that recording only**, for 6 hours; a new `upload` command for the same recording reuses
  the same upload (resume) with a fresh token.
- The racket acks `received` at once and `applied` when the backend confirmed completion (§2.3), or `rejected`
  with `reason`: `busy` (recording), `unknown_recording`, `io_error`, `network`.

### 2.2 The racket sends (HTTP, header `Authorization: Bearer <token>`)

| Step | Request | Answer |
|---|---|---|
| 1. where to resume | `GET  {url}` | `200 {"received_bytes": N, "size": S|null}` |
| 2. each chunk | `PUT  {url}?offset=N` body = bytes `[N, N+len)`, header `X-Chunk-CRC32: <8 hex>` | `200 {"received_bytes": N+len}` |
| 3. finish | `POST {url}/complete` body `{"size": S, "sha256": "<64 hex>"}` | `200 {"status": "verified", ...}` |

- **Chunks:** `chunk_bytes` (512 KB default), sent in order. A chunk whose CRC does not match → `422`, re-send it.
  `offset` lower than `received_bytes` → the overlap is ignored (idempotent retry); higher → `409
  {"received_bytes": N}`: continue from `N`.
- **On any network error:** wait (backoff 2, 4, 8 … 60 s), then step 1 and continue from `received_bytes`.
  Wi-Fi drops cost one chunk at most.
- **Finish** returns only after the backend has hashed the file and checked every block. Answers:
  - `200 {"status":"verified","blocks":…,"samples":…,"bad_blocks":0}` → done;
  - `422 {"status":"sha_mismatch"}` → the backend discards the upload; restart from offset 0;
  - `200 {"status":"verified_with_errors","bad_blocks":k}` → stored, k blocks failed CRC (a NAND problem worth
    seeing — the dashboard flags it).
- Upload **while recording** is not required. Uploading a previous recording during a new one is allowed only if the
  firmware is sure it cannot delay the 1 kHz logging; otherwise reply `busy`.

### 2.3 Deleting on the racket

After `verified`, the backend sends `delete_recording {rec_id}` (protocol-v2 §8). The racket deletes the file and
re-publishes its `hello` (inventory changed). **ASSUMPTION: auto-delete after verification is ON by default** —
the NAND holds ~100 MB (≈ 80 min at 20 B × 927/s); a dashboard setting can turn it off per racket.

---

## 3. What the backend does with it

- Raw file kept **as received** (the original, never modified): `/opt/perceptio2/data/recordings/<device>/<rec_id>.pcr2`.
- On verification it decodes every block into an indexed store (per-sample time in µs + every field) and computes:
  measured rate, timing jitter, gaps (from `dt_us` and the `flags` bit), samples, duration, clock drift (start/stop
  anchors vs monotonic time), peaks per channel, and **saturation counts** per sensor (samples at full scale).
- Viewer: every channel, zoom and pan from the whole recording down to single samples (min/max decimation, so short
  impacts are never hidden by down-sampling), plus the raw file download.

## 4. Open questions for the firmware

1. The real sample layout and scale factors (the header carries them — no backend change needed either way).
2. Sensor part numbers and axis orientation on the board.
