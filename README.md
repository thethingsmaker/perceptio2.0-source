## Layout (block 1 — modular)

```
src/main.c          boot sequence, change hook, 100 ms tick; passes hooks to the policy modules below
src/board.c         pins + I2C0/SPI2/SPI3 bus init, GPIO ISR service installed from core 1
src/bq25180.c       charger: init (ICHG from parameter ichg_ma, default 300 mA; watchdog off), status, ship mode
src/lp5810.c        LED driver: init, brightness, rgb
src/imu.c           LSM6DSV320X via the ST driver (components/lsm6dsv320x), burst read
src/w25n01kv.c      NAND: init + bad-block scan, erase/program/read with ECC status
src/power.c         VBAT (eFuse-calibrated ADC), BQ /INT -> button hook
src/button.c        button gestures: double press -> start/stop recording, hold 3 s -> power off
src/power_mgr.c     power states (ON / OFF = ship mode / CHARGE-IDLE), charge gate, low-battery shutdown, boot report
src/netwatch.c      fallback AP: up when no network joins or the backend is unreachable 90 s, off when it is back
src/led_policy.c    status LED priority list (see LED)
src/diag.c          temporary: once-a-minute stack report (DIAG line), idle only
src/net.c           WiFi join from NVS list, keepalive, mDNS
src/sampler.c       core-1 DRDY-driven sampler with sinks (store_feed, webui_feed)
src/store.c         recording store: PCR2 files on the NAND (one block per page), core-1 writer + SHA-256, inventory
src/upload.c        resumable HTTPS upload of a recording, driven by the backend's `upload` command
src/selftest.c      the bring-up tests on top of the drivers; results on /api/v1/selftest
src/config.c        NVS + self-describing parameter registry
src/webui.c         REST /api/v1 + WebSocket + embedded page (src/web_index.c from web/index.html)
partitions.csv      nvs, otadata, phy, ota_0 (1.875 MB), ota_1 (1.875 MB), coredump
```

## Persistent configuration (NVS)

Namespace `cfg`. Module: `include/config.h`, `src/config.c`.

- `fw_version` — mirrored from `FW_VERSION_STR` in `config.h` at every boot.
- `wifi_list` — blob, up to 8 `{ssid, pass}` tried in order at boot. Seeded from `include/wifi_defaults.h`
  (git-ignored) when absent; afterwards owned by the UI/app.
- **Parameters** — one NVS entry per key, declared in `PARAM_TABLE` in `config.c` with type, range, options,
  unit, label, group and a `reboot` flag. The web page and the mobile app build their settings form from
  `GET /api/v1/schema`, so adding a parameter is one line in the table and nothing else.
  Live-applied parameters go through the change hook in `main.c` (`on_cfg_change`).
- NVS is written only at boot or after a session stops — never during capture.

## Web app and API

After the tests, if a saved network was joined, the board stays connected and starts:
- the HTTP server on **core 0** (`src/webui.c`), serving the embedded web app (`web/index.html`, gzipped at
  build time by `tools/gzip_web.py` into `web/index.html.gz`, ~7 KB) and the JSON API in **`API.md`**;
- an IMU sampler task on **core 1** (DRDY interrupt → SPI polling read, 960 Hz) feeding a lock-free ring
  that the WebSocket sender (core 0) flushes at `ws_hz` frames/s.
- mDNS: `http://<device-name>.local/` and service `_perceptio._tcp` for app discovery (component
  `espressif/mdns`, pulled by `src/idf_component.yml` on first build — needs internet once).

**Fallback access point** `Perceptio-<device id>` (e.g. `Perceptio-d40592485f78`), WPA2 password `perceptio`, same
web app at `http://192.168.4.1/` (Network tab to add a WiFi). It comes up (`src/netwatch.c`) when no saved network
joins at boot, or when the backend has been unreachable for 90 s (WiFi without internet, router gone...). While it is
up and no phone is connected, the saved networks are retried every 60 s. Once the backend has been reachable for 30 s
the AP is switched off; with a phone connected it stays up and is retried every 30 s. The password is the same on
every unit — development only, to be replaced before shipping.

The page works on phones (bottom tab bar), follows the system dark/light theme, and needs no internet.
Tabs: Dashboard · Live (60 fps plots) · Settings (generated from the schema) · Network.

Core affinity: `sdkconfig.defaults` pins main, WiFi, lwIP and esp_timer to core 0; `httpd` has `core_id = 0`;
the sampler is `xTaskCreatePinnedToCore(..., 1)` and installs the GPIO ISR service from core 1.

## LED (`src/led.c`, decided in `src/led_policy.c`)

One colour at a time, highest priority first; re-evaluated every second. Each change logs `LED: <state>`.

| Priority | State | LED |
|---|---|---|
| 1 | identify (backend command) / recording refused | white fast blink |
| 2 | self-test failed | red fast blink, 3 s after boot |
| 3 | recording | red breathing |
| 4 | charge fault: paused by temperature, safety timer, cable + cell but not charging for 10 s | yellow fast blink |
| 5 | fallback AP up (connect a phone now) | blue fast blink |
| 5b | WiFi or backend unreachable ≥ 20 s, AP not up yet (it follows at 90 s) | blue slow blink (1 Hz) |
| 6 | charge done (cable + cell) | yellow solid |
| 7 | charging | yellow breathing |
| 8 | WiFi + backend OK, idle | green breathing |

While booting: blue breathing. Device off with the cable in (CHARGE-IDLE): yellow breathing / solid / fast blink as above.
No battery connected: no charging states (the charger toggles; see `power_battery_present()`).

## Power (`src/power_mgr.c`, button in `src/button.c`)

- Button (the BQ25180 push-button): two presses within 1.5 s start/stop a recording; holding 3 s switches off; a
  single press does nothing. Switching off can also come from `POST /api/v1/power/off` or the low-battery rule below.
  Every switch-off first closes a running recording, tells the backend (`shutdown`) and marks a fresh OTA image valid.
- **OFF = BQ25180 ship mode**: the cell is disconnected from the system, nothing runs, the clock is lost. A button
  press or plugging the cable wakes it (boot with `reset poweron`).
- **Cable in**: ship mode cannot engage while VIN is present (the charger leaves it), so the board reboots into
  **CHARGE-IDLE**: LED shows the charge state, no WiFi/UI/sampler. A button press turns it on (software restart,
  `reset sw`); pulling the cable switches it off into ship mode.
- **Low battery** (0.3.33): on battery, when the averaged VBAT stays at or below parameter `bat_off_mv` (Power group,
  default 3300 mV, range 3100–4200) for 30 s, the board switches off as above (`PWR: battery low …`). Without it a
  flat board browns out at the charger's 3.0 V BUVLO, the unloaded cell recovers, it boots, and repeats.
- **Boot report** (0.3.34): a breadcrumb in RTC memory (survives resets and deep sleep, not ship mode) tells at the
  next boot how the last switch-off ended:
  - `PWR: last power-off reached ship mode (supply was cut)` — normal.
  - `PWR: last power-off fell back to deep sleep: ship mode did NOT engage (woken by …)` — ship mode was requested but
    the supply was still on after 6 s, so the firmware went to deep sleep (60 s timer) instead. A timer wake with
    nobody pressing tries ship mode again and sleeps 600 s, so the board does not turn itself on.
  - `PWR: last power-off was interrupted while waiting for ship mode` — reset during the 6 s wait.
  Verified 2026-10-29 (board …fcc): ship mode engages on battery, and after CHARGE-IDLE when the cable is pulled.
- Charge gate: no NTC, so the board temperature is the ambient proxy: charging pauses outside `chg_tmin_c`..
  `chg_tmax_c` and resumes with 5 K hysteresis; a BQ safety-timer fault restarts the charge cycle.

## Recording store (`src/store.c`) — PCR2 on the NAND

- Files are **PCR2** (`docs/data-v2.md`), written directly on the W25N01KV, no filesystem: file header (JSON padded
  with spaces to fill the first 2032 B), then **codec-1 blocks of 400 samples** (delta bit-packing, `src/pcr2_codec1.c`
  from the backend, ~3.6× smaller than raw on real data), then the END2 record. The stream is cut into 2032 B slices:
  every NAND page = 16 B container header (`include/store.h`: recording id, page index, stream bytes, stream offset) +
  stream bytes; all pages are full except the last, so page = offset / 2032 and the upload is the byte-for-byte stream.
  Blocks 8..1019 for data, 0..7 / 1020..1023 reserved (self-test scratch).
- Sample = `dt_us u16` (from the DRDY-ISR timestamp) + `gx gy gz a1x a1y a1z a2x a2y a2z i16`; the header's
  `sample.fields` carries the real scales from the ST driver (gyro 0.035×FS mdps/LSB, accel 0.488 mg/LSB @±16 g, high-g
  10.417 mg/LSB @±320 g), `sensors` the part (LSM6DSV320X for all three) and ranges, `nominal_rate_hz` the rate the
  sampler measures at start (~927 — the sensor's internal oscillator runs ~3.4 % slow at the 960 Hz ODR setting, which
  is in `sensors.*.odr_hz`; the backend's viewer compares against `nominal_rate_hz`).
  Temperature is not logged (`temp_c_start` / `temp_c_stop` in header / END2). A gap > 65.535 ms or a ring-full drop
  closes the block; the next block carries `flags` bit0 after a drop. CRC-32 per block over the encoded bytes
  (self-tested at boot against `0xCBF43926`).
- Pipeline: `store_feed()` in the sampler fills raw 400-sample blocks in a 16-block PSRAM ring (~6.9 s of slack); the
  core-1 writer encodes each block, appends it to a page assembler, programs full pages (pre-erase one block ahead,
  bad-block skip, wear rotation) and hashes the stream (mbedtls SHA-256) as it goes. The digest is stored in the END
  page outside the stream (or in one extra page), so complete files never need re-reading for the upload; incomplete
  ones (power cut: no END2, ≤ ~1 s lost) are walked block by block at boot and hashed when first uploaded.
- Rules: no internal-flash writes while recording (`config_set_locked`: PATCH /config and WiFi edits → 409); NAND reads
  (download, upload, delete) refused while recording — a recording starting mid-upload aborts the upload with `busy`;
  the session is closed with END2 before power-off, at `session_max` minutes and when the storage is full.
- Capacity with codec 1: roughly **4–5 h** on 126 MB (raw would be ~110 min).
- `GET /api/v1/recordings/<rec_id>` serves the PCR2 file (`.pcr2`); validate with the backend's `tools/pcr2_check.py`
  (needs numpy; `tools/pcr2_dump.py` handles codec 0 only).
- Verified on the host against a NAND model (`pcr2_check.py` exit 0, SHA identical, forced gap/drop/power-cut/reboot).

## Upload (`src/upload.c`) — resumable HTTPS (`docs/data-v2.md` §2)

- Started only by the backend's `upload` command (`rec_id, url, token, chunk_bytes`); ack `received` at once, then
  in its own core-0 task: `GET url` → `received_bytes`; `PUT url?offset=N` per chunk (PSRAM buffer, `X-Chunk-CRC32`,
  `Authorization: Bearer`, `User-Agent: Perceptio/<fw> (<device_id>)`, persistent TLS connection); `POST url/complete
  {size, sha256}` → ack `applied` with the server's verification JSON as `result`. 409 → continue from the server's
  offset; 422/5xx → resend; `sha_mismatch` → restart from 0 (twice, then `io_error`); network errors → back off
  2…60 s and resume, giving up after 1 h (`network`). Rejections: `busy` (recording / another upload),
  `unknown_recording`, `io_error`, `network`.
- State `uploading` while it runs; Wi-Fi modem sleep off for its duration; `hello` re-published every 5 s with
  `uploaded_bytes`; `/api/v1/status.upload` and the Record tab show phase and progress. Every chunk logs NAND read time,
  send+response time and KB/s; the `applied` ack carries `result.stats` `{chunks, bytes, total_ms, read_ms, send_ms,
  connects, retries, rssi, phy, heap_min_kb, tcp_snd_buf, tls_out, drops[{offset, step, err, errno, rssi}]}` and every
  drop also emits an `upload_retry` event. NAND reads are double-buffered (a reader task fills the next 512 KB chunk
  while the current one is on the wire). Backoff after a drop: 0.5 s doubling while Wi-Fi is up, 2 s doubling when down.
  `upload_init` logs the effective build values (TCP windows, mailbox sizes, TLS allocation and record size).
- Measured ceiling (2026-10-23, rev 20, RSSI −45..−52): ~530 KB/s, no drops; a 40 MB codec-1 match in ~75–90 s.
  Internal heap during an upload: ≥ ~100 KB since 0.3.25 (it was 9–19 KB). The levers and dead ends are in CLAUDE.md
  ("Memory and upload throughput"). Throughput settings in `sdkconfig.defaults` (64 KB TCP send buffer, 32 KB receive
  window, Wi-Fi/lwIP buffers in PSRAM, 16 KB TLS records) per `docs/firmware-upload-speed.md`. The racket never deletes on its own — `delete_recording` does.

## Firmware update (`src/ota.c`) — GitHub Releases

Settings → Firmware update, or `POST /api/v1/ota/check` + `/install`. Source = the latest Release of the repository in
parameter `ota_repo` (default `thethingsmaker/perceptio2.0-firmware`); the first asset whose name ends in `.bin` is the
image. Private repo: store a GitHub token once (Settings → GitHub token → NVS `ota_pat`; a fine-grained token with
*Contents: read* on that repository is enough). The token is never returned by any API and never logged.

Flow: `GET api.github.com/repos/<repo>/releases/latest` → `tag_name`, asset API url → `GET <asset url>` with
`Accept: application/octet-stream` and `disable_auto_redirect` → the 302 `Location` (signed CDN URL) is fetched **without**
the Authorization header (the CDN rejects two credentials) → `esp_https_ota` into the spare slot → reboot. The new image
boots `PENDING_VERIFY` (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main` marks it valid once the cloud is online (or
after 90 s offline), never during a session; any reset before that rolls back to the previous slot. Deliberate restarts
(UI Restart, Power off) mark the image valid first. First boot after an update publishes `fw_updated {from, to}`.

Refused while recording or uploading (internal-flash rule). **Update mode** (0.3.21+): a check or an install first tells
the backend `state off (fw_update)` and destroys the MQTT client, so the job runs with ~145 KB internal RAM (0.3.25+);
a failed job brings the link back (`CLOUD: resumed`), an installed image reboots. The job needs 24 KB internal free
(`MIN_HEAP`, measured inside the job task). Every step logs the free heap; reads that block > 2 s log `slow read`.
The pending-verify state is read from otadata once at boot (`ota_pending_verify()` is a RAM flag): reading otadata in the
main loop stalled both cores 10x/s and caused interrupt-watchdog resets mid-recording until 0.3.20.

Publishing a release: bump `FW_VERSION_STR` in `include/config.h` to match the tag (`v0.3.1` ↔ `0.3.1`), build, attach
`.pio/build/perceptio2/firmware.bin` to a GitHub Release with that tag. If the tag and `FW_VERSION_STR` differ the UI
keeps reporting "update available".

## Remote log (`src/rlog.c`)

- Every `ESP_LOGx` line is captured by an `esp_log_set_vprintf` hook into a 32 KB PSRAM ring and published by the cloud
  task on `perceptio/2/<id>/log` — **QoS 0, plain text, newline-separated IDF lines (`I (12345) TAG: msg`), ≤ 2 KB per
  message, about once a second when there is something**. Boot lines wait in the ring until the session is up; offline the
  ring keeps the newest ~32 KB (older lines dropped, count in `rlog_dropped()`). A crash loses the lines after the last
  flush (the coredump partition covers panics: the dump is kept until `DELETE /api/v1/coredump`, see API.md).
- Parameters (group Debug): `log_serial` (USB console on/off — off for the ship build), `log_remote` (0 none, 1 error,
  2 warning, 3 info = default). Passwords are never logged.
- Backend contract: subscribe `perceptio/2/+/log`, append to a per-racket file, show a tail in the dashboard.

## Cloud link (`src/cloud.c`)

Implements `docs/protocol-v2.md` (device ↔ backend control, MQTT 5 over WSS at `mqtt.padelsense.net`, TLS verified
with the IDF certificate bundle). Runs entirely on core 0 (esp-mqtt task pinned there) and never touches the capture path.

- Identity: `device_id` = eFuse MAC (12 hex). QR payload `P2:<id>` is in `/api/v1/info.qr`.
- Not enrolled → connects as `enrol` (password in git-ignored `include/cloud_secrets.h`), publishes the request with a nonce,
  waits for approval in the dashboard (`admin.padelsense.net/p2/`), stores the per-racket credential in NVS namespace `p2`.
- Enrolled → persistent session, last will `{"online":false}`; on every connect: subscribe `cmd`, publish retained `hello`
  and `state`, replay buffered events. Telemetry every 10 s; SNTP every 10 min.
- Every completed NTP sync (boot, periodic, `sync_time`) emits a `time_synced` event with `data.offset_ms`. The offset is
  `null` on the first sync after a cold start (the clock was unset, so there is no meaningful correction); the same rule
  applies to `hello.clock.offset_ms` / `last_sync_utc_ms`: unknown is `null`, never a placeholder number.
- Commands: `identify` (white LED flash), `sync_time`, `reboot`, `start_recording` (`rec_tag`, optional `rate_hz` which must
  equal the active ODR — the ODR is a reboot parameter; result `rec_id`), `stop_recording` (optional `rec_id` must match;
  result `rec_id, samples, bytes, duration_ms`), `upload` (see below), `delete_recording` (`rec_id`); others rejected `unsupported`. Refusals use the
  spec reasons: `busy`, `not_recording`, `storage_full`, `battery_low` (< 5 % on battery), `bad_args`. Duplicates (64-id history)
  re-ack the original outcome; `expires_utc_ms` honoured when the clock is synced.
- Three consecutive connection refusals with the racket credential → credential erased → back to enrolment.
- Deviations from the spec, by hardware: `caps.rate_hz` is `[960]` (one LSM6DSV320X, 480/960/1920 ladder — no 1000 Hz);
  `caps` also carries the sensitivities (`mg_per_lsb`, `gyro_mdps_per_lsb`); `hw_rev` is `"20"`.
- `hello` is re-published (retained) whenever the inventory changes — recording start/stop, delete, later upload
  progress — via `cloud_inventory_changed()`, so the broker's stored copy is never stale.
- `hello` carries `current_rec` while recording and the `recordings` inventory with live `uploaded_bytes`; `state`
  carries `rec_id` while recording and is `uploading` during an upload. Events `rec_started` `{rec_id, by, rec_tag}` and `rec_stopped`
  `{rec_id, by, samples, bytes, duration_ms, dropped, complete}` — `by` ∈ `button`, `ui`, `cloud`, `timeout`, `storage_full`, `power_off`.
- Dashboard card "Cloud" and `/api/v1/status.cloud` show state, enrolment, boot id, queued events, last command.
- Intentional power-off (button): event `shutdown` `{"by":"button"}` + retained state `{"online":false,"state":"off","reason":"button"}`,
  PUBACK awaited, then a clean DISCONNECT — so the backend can distinguish "switched off" from "lost" (last will).
  *Backend side:* accept event type `shutdown` and the `reason` field on `state` (both additive).
