# Firmware note — make uploads much faster (2026-10-22)

Paste to the firmware Claude. Attach: `pcr2_codec1.c`, `data-v2.md` (new §1.4), `make_pcr2.py`, `pcr2.py`,
`pcr2_check.py`.

## What we measured
- **The racket uploads at ~106 KB/s.** Padel 0's 44 MB recording `24-0` took 89 chunks of 512 KB, median **4.83 s per
  chunk** (min 3.34, p90 5.29, max 6.40), so about 7 minutes.
- **The server path is not the bottleneck.** A client on the server, sending the same 512 KB chunks through Cloudflare to
  the same ingest URL, took **0.17 s per chunk (~3 MB/s)**. That is about 30× more headroom, so the time is spent on the
  racket: NAND read, TLS, TCP/Wi-Fi, or the HTTP client.
- **The data compress 3.6× losslessly** (the new codec 1, below).

Two levers, both needed: **send fewer bytes** (codec 1, certain gain) and **send them faster** (network tuning; the
causes are hypotheses until your timings confirm them).

## 1. Measure first (small, do it now)
For every chunk, log `nand_read_ms`, `connect_ms` (0 if the connection was reused), `send_ms`, `response_ms` and
`bytes`. In the `applied` ack of the `upload` command, add to `result`:
`"stats": {"chunks": N, "bytes": B, "total_ms": T, "read_ms": R, "send_ms": S, "connects": C, "retries": K}`.
The backend stores the whole ack, and it shows in the dashboard's Commands list. That tells us which fix worked.

## 2. Send them faster — likely causes, most likely first
1. **One TLS connection per chunk.** A new TLS handshake on the S3 (ECDHE plus certificate check) costs about 0.5–2 s.
   Create **one** `esp_http_client` for the whole upload with `keep_alive_enable = true`. For each chunk call
   `esp_http_client_set_url()` and `set_method()`, then `open` / `write` / `fetch_headers` / `read` the body /
   **`esp_http_client_close()` without `cleanup()`**, so the socket stays open. Reconnect only after an error.
   Cloudflare keeps idle HTTPS connections alive.
2. **TCP window too small for the round trip.** The ESP-IDF defaults are `LWIP_TCP_SND_BUF_DEFAULT = 5744` and
   `LWIP_TCP_WND_DEFAULT = 5744`. With one ~5.7 KB window per round trip and a 30–50 ms RTT to Cloudflare, the ceiling
   is ≈ 110–190 KB/s, which is exactly what we see. In `sdkconfig`:
   ```
   CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
   CONFIG_LWIP_TCP_WND_DEFAULT=65534
   CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
   CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64
   CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=64
   CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y      # Wi-Fi/lwIP buffers in PSRAM: internal heap is already low
   CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y         # TLS buffers in PSRAM (see findings note #2)
   CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=16384    # full-size TLS records out
   ```
   These follow the ESP-IDF `iperf` example's settings. Check that internal heap stays healthy after the change.
3. **Wi-Fi modem sleep.** The default `WIFI_PS_MIN_MODEM` delays received ACKs until the next DTIM beacon, which stalls
   TCP. Call `esp_wifi_set_ps(WIFI_PS_NONE)` when the upload starts and restore the previous mode when it ends. Battery
   cost: only for the length of the upload.
4. **Write in big slices.** Hand `esp_http_client_write()` 16–32 KB at a time from the PSRAM chunk buffer, not small
   pieces. Set `buffer_size_tx` to at least 4096.
5. **NAND read and send in series.** If `nand_read_ms` is large, use two 256 KB PSRAM buffers. The writer task reads
   chunk *k+1* while the upload task sends chunk *k*.

Target after these: **500 KB/s to 1 MB/s or more** on good venue Wi-Fi. The backend accepts chunks up to 4 MB. Keep
512 KB unless your timings show a reason to change it.

## 3. Send fewer bytes — codec 1 (backend ready and live)
- In each block header write **`codec = 1`** and encode the payload with **`pcr2_codec1_encode()`** from
  `pcr2_codec1.c` (portable C, no allocation, no library). It is byte-for-byte identical to the backend's reference
  `make_pcr2.encode_c1`: 0 mismatches in 400 random layouts, compiled and tested.
- Do the encoding **in the writer task when a block is written to NAND**, never in the sampling path. The upload stays
  a byte copy of the file. The CRC-32 and the incremental SHA-256 are over the **encoded** bytes, so `payload_len`
  becomes the encoded length.
- Output buffer worst case: `n·sample_size + 2·n_fields·ceil(n/32)` bytes. That is 2,080 B for 100 samples × 20 B × 10 fields.
- Blocks get about 3.6× smaller (100 samples → ≈ 550 B), so 32 bytes of block header is now ~6 % overhead.
  **Recommend 400 samples per block** (≈ 2.2 KB encoded), still small enough to lose little on a power cut.
- Real data: **46.5 MB → 12.8 MB** for recording 24-0. The same NAND now holds about **290 minutes instead of 80**.
- Validate: dump one codec-1 file and run `python3 pcr2_check.py FILE.pcr2` (exit 0). The backend decodes codecs 0 and 1,
  even mixed in one file, so older files on the racket still upload fine.

## Expected result for a 40-minute session (44 MB raw today)
| | bytes | at today's 106 KB/s | at 500 KB/s | at 1 MB/s |
|---|---|---|---|---|
| today (codec 0) | 44 MB | **~7 min** | 1.5 min | 44 s |
| codec 1 | 12.8 MB | 2 min | 26 s | **~13 s** |

## Acceptance
1. A codec-1 recording passes `pcr2_check.py`. On the dashboard it shows **verified** with the same sample count as the
   racket's counters.
2. The upload's `applied` ack carries `stats`. Report `bytes / total_ms` before and after each change from §2.
3. 0 dropped samples while recording with codec 1 (check `dropped` in `rec_stopped` and the viewer's gap count).
4. Wi-Fi drop mid-upload still resumes from `received_bytes`, now over a fresh keep-alive connection.
