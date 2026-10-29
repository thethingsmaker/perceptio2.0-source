# Perceptio 2.0 — device API v1

Base URL: `http://<ip>/api/v1` (also `http://<name>.local/api/v1` via mDNS; service `_perceptio._tcp` for discovery, TXT `fw`, `api`, `model`).
All bodies are JSON (`Content-Type: application/json`). CORS is open (`*`), preflight handled on `/api/*`.
No authentication yet (LAN only) — a bearer token will be added before release; clients should send
`Authorization: Bearer <token>` once `/info` reports `"auth": true`.

The embedded web page uses exactly this API; the mobile app must not need anything else.

## Identity and status

| | |
|---|---|
| `GET /info` | static identity: `name, model, fw, hw, api, mac, ip, title` ("Perceptio2.0-<last IP octet, 3 digits>-<yyy, reserved: 000>"), `idf, chip_rev, odr_hz, xl_fs_g, gy_fs_dps` (the last three are the **active** IMU configuration — use them to scale the live stream) |
| `GET /status` | live numbers, poll at ≤ 2 Hz: `uptime` (s), `heap_free`, `heap_min` (bytes), `vbat` (V), `vbat_cal` (bool: eFuse-calibrated ADC), `soc_pct` (0–100, estimated from the averaged voltage and rate-limited to 1 %/10 s — no fuel gauge; `null` when no battery is connected), `battery` (bool: false = no cell, USB power only), `vin` (bool, USB present), `chg` (0 not charging · 1 CC · 2 CV · 3 charge done — also reported when no battery is connected), `rssi` (dBm), `ip`, `title` (browser title, follows the IP), `imu_hz`, `imu_total`, `imu_missed` (DRDY timeouts — must stay 0), `imu_temp` (°C), `accel_mg[3]`, `gyro_dps[3]`, `hg_mg[3]` (high-g channel), `hg_peak_g` (max |a_hg| over the last second), `time` `{valid, epoch, local, tz, source: none/ntp/app/rtc, last_sync, age_s}`, `rec` `{state: idle/recording/stopping, rec_id, rec_tag, elapsed_ms, samples, dropped, pages, backlog_max (while recording), count, total_mb, free_mb}`, `upload` `{phase: idle/resuming/sending/verifying/backoff, rec_id, sent_bytes, total_bytes, retries, last_error}`, `tests_pass`, `tests_total` |

## Parameters (self-describing)

| | |
|---|---|
| `GET /schema` | array describing every editable parameter — **render the settings UI from this**, never hard-code keys |
| `GET /config` | current values `{key: value, …}` |
| `PATCH /config` | body `{key: value, …}` (any subset). Validated per key. Response: full config + `applied` (count) + `errors` `{key: reason}`. HTTP 400 only if nothing applied and there were errors. |

Schema entry:

```json
{ "key":"odr_hz", "label":"IMU sample rate", "group":"Capture", "unit":"Hz",
  "type":"int", "min":400, "max":1600, "options":[400,800,1600], "reboot":true }
```

- `type`: `int` | `bool` | `str` (`max` = max length for `str`)
- `options`: present → the only allowed values (render a picker); absent → any int in `[min,max]`
- `reboot`: `true` → saved now, applied at next boot (show a "restart to apply" hint); `false` → applied immediately
- Values are persisted in NVS on the device. **Writes are refused during a capture session** in the production firmware (HTTP 409) — the app should retry after the session ends.

Current parameters: `dev_name, odr_hz, xl_fs_g, gy_fs_dps, hg_fs_g, session_max, auto_upload, led_bright, wifi_ps, ws_hz`.

## WiFi networks

| | |
|---|---|
| `GET /wifi` | `[{ssid, open, connected}]` in boot priority order (first = preferred); passwords are never returned |
| `POST /wifi` | `{ssid, pass}` — add, or update the password if the SSID exists; returns the list |
| `DELETE /wifi/<url-encoded ssid>` | remove; returns the list |

Max 8 networks. SSID ≤ 32 bytes, password ≤ 64.

## Control

| | |
|---|---|
| `GET /wifi/scan` | nearby networks, blocks ~2–3 s: `[{ssid, rssi, channel, open}]`, unique SSIDs, strongest first, hidden ones omitted. 409 while a connect attempt is running |
| `POST /wifi/test` | `{ssid, pass}` → 202: joins that network, waits for DHCP, resolves the backend host, saves it **only on success**, then returns to the previous network (with none — fallback AP — it stays on the new one). Takes 5–40 s; while connected elsewhere the board is away from the current network meanwhile. 409 while recording/uploading/updating or another test runs; 400 for a password of 1–7 chars |
| `POST /wifi/connect` | `{ssid}` of a saved network → 202: switch to it now (same steps as the test; stays there if it works, nothing saved; on failure returns to the previous network). Progress in `GET /wifi/test` with `mode:"connect"`. 404 not saved, 409 already connected / busy / recording |
| `POST /wifi/prefer` | `{ssid}`: moves it to the top of the saved list (tried first at boot and by the fallback-AP retries); answers like `GET /wifi` |
| `GET /wifi/test` | `{state: idle/running/ok/failed, ssid, step?, ip?, rssi?, internet?, saved?, error?, reason?, current_ssid}` — poll it; requests fail while the board is away from the network |
| `POST /reboot` | `{"ok":true}` then restart ~200 ms later |
| `GET /ota` | `{phase, running, slot, pending_verify, repo, token_set, token_hint, checked, latest?, asset?, asset_bytes?, published?, update_available?, received_bytes?, total_bytes?, error?}` — phase: idle, checking, downloading, verifying, rebooting, error |
| `POST /ota/check` | asks GitHub for the latest Release of parameter `ota_repo`; answers like `GET /ota` (poll it while `phase != idle`). 409 while recording/uploading/busy |
| `POST /ota/install` | downloads the `.bin` asset of the last check into the spare slot, verifies, reboots (~30 s offline). 409 without a prior check. The new image must be confirmed by a healthy boot or the bootloader rolls back |
| `POST /ota/token` | `{"token":"ghp_…"}` stores the GitHub token in NVS (`""` clears). Never returned: `GET /ota` gives `token_hint` ("ghp_…1234") only |
| `POST /power/off` | `{"ok":true,"mode":"ship"\|"charge_idle","recording_closed":bool}` — same as the 5 s button press: closes a running recording, notifies the backend (`shutdown`), then ship mode; with a cable in, reboots into CHARGE-IDLE (UI off, charging on; 2 s button press turns it back on) |
| `GET /selftest` | results of the self-test run at boot: `{passed, total, results:[{name, pass, info}]}` |
| `GET /coredump` | the last crash's core dump image as a file (`coredump-<device_id>-<fw>.bin`); 404 when there is none. Decode: `esp-coredump info_corefile -t raw -c <file> firmware.elf` (the ELF of the build that crashed). 409 while recording (flash read) |
| `DELETE /coredump` | erases it: `{"erased":true}`. 409 while recording. A new crash overwrites it anyway |
| `POST /time` | `{"epoch": <seconds since 1970, UTC>}` — the app should call this on every connection when `status.time.valid` is false or `source` is not `ntp` (no backup battery: time is lost in ship mode). Returns `{epoch, local}`. |

## Recordings (NAND store)

| Call | Returns |
|---|---|
| `GET /recordings` | `{state, nand_ok, current?, total_mb, free_mb, recordings:[…]}`. Each recording: `rec_id` (`<device_id>-<boot_id>-<n>`), `rec_tag`, `started_utc_ms` (null if the clock was not set), `duration_ms`, `samples`, `dropped`, `saturated`, `blocks`, `bytes` (file size), `uploaded_bytes`, `rate_hz`, `complete` (false = power was lost while recording; the data up to the last page is still valid), `this_boot` |
| `POST /recordings/start` | body `{"rec_tag": "…"}` (optional, ≤ 64 chars). `{rec_id}`. 409 already recording, 507 storage full. Parameters and WiFi settings are locked (409) until the recording stops. |
| `POST /recordings/synthetic` | body `{"minutes": 150, "rec_tag": "…"}` — test aid: writes a synthetic recording of that length through the normal pipeline at NAND speed (~1 min per 150 min), then stops it like any recording (events, upload). 409 while recording. |
| `POST /recordings/stop` | the finished recording (as above). 409 if idle. Takes up to ~1 s (flush + trailer). |
| `GET /recordings/<rec_id>` | the recording as a **PCR2** file (`docs/data-v2.md`), `application/octet-stream`, `<rec_id>.pcr2` — byte-identical to what the upload sends. Validate with `tools/pcr2_dump.py`. 409 while recording. |
| `DELETE /recordings/<rec_id>` | erases it; returns the list. 409 while recording. |

A recording also starts/stops with a **double press** of the racket button (two presses of ≥ 0.3 s each within 1.5 s; a single press does nothing, holding 3 s powers off) and from the backend (`start_recording` / `stop_recording`
commands); the UI shows whoever started it. Capacity at 960 Hz × 20 B: ~1.15 MB/min → ~110 min on the 126 MB usable
(480 Hz doubles it). The session stops by itself at `session_max` minutes or when the storage is full.

## Live stream

`ws://<ip>/ws` — binary frames, flushed every `1000/ws_hz` ms (default 60 fps), up to 4 clients.

```
uint32  seq        index of the first sample in this frame (detect gaps: next seq == seq + n)
n ×  int16[10]     TEMP, GX, GY, GZ, AX, AY, AZ, HGX, HGY, HGZ   raw LSB, little-endian, sample_bytes (20) each
```

Scaling (from `/info`): accel g = `raw * xl_mg_lsb / 1000`; gyro dps = `raw * gy_mdps_lsb / 1000`;
high-g g = `raw * hg_mg_lsb / 1000`; temp °C = `25 + raw / 256`.
The sensitivities are the ST datasheet values, **not** `full_scale / 32768`: gyro is 0.035 × FS mdps/LSB (140 at ±4000, register
spans ±4587 dps) and high-g is 10.417 mg/LSB at ±320 g (spans ±341 g). Use `sample_bytes` for the stride; fields are only ever appended.
The client sends nothing; anything it sends is discarded.

## Error shape

Non-2xx responses carry `{"error": "<text>"}`.

## Versioning

Breaking changes bump the path (`/api/v2`). Additive fields may appear in any release; clients must ignore unknown fields.
