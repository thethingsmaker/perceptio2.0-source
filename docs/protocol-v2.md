# Perceptio 2.0 — device ↔ backend control protocol

Status: **draft 1, 2026-10-22**. Covers how a 2.0 racket identifies itself, enrols, announces
itself, reports state, and receives commands. **Bulk recording upload is out of scope** (next spec).

Items marked **ASSUMPTION** are defaults chosen without an explicit decision — overrule freely.

---

## 1. Ground facts

| | |
|---|---|
| MCU | ESP32-S3, dual core, **ESP-IDF** |
| Sensors | 1 gyro + 2 accelerometers (**±16 g** and **±320 g**), **1 kHz** |
| Storage | external **SPI NAND flash** (capacity: TBD) |
| Wi-Fi | stays up while logging, **but may drop at any time, including the whole match** |
| Live link | **control and feedback only** — commands, acks, state, telemetry. Never sample data. |
| Backend | new service on the padelsense.net server (46.224.113.25), alongside nothing of v1 |

Data budget, for scale (to be re-measured on real files): one sample ≈ 20–24 B → ~22 KB/s raw,
~1.3 MB/min, **~120 MB raw / ~40–60 MB compressed per 90-min match per racket**.

## 2. Design rules (lessons from v1)

1. **The racket owns the recording.** Start, stop, record and store work with no network at all.
2. **Nothing joins the fleet unapproved.** No auto-registration on first contact (v1 accepted any serial).
3. **Every message is authenticated** with a per-racket credential (v1 never checked its API key).
4. **Commands expire and are acknowledged.** v1 commands had no expiry and once started a recording
   hours after the match.
5. **Recordings carry identity.** The backend's match reference is stamped into the recording at
   start, so linking is by id, not by guessing from timestamps (v1's `_link_sessions` heuristic).
6. **Clocks are UTC and measured.** v1 rackets ran local time while the match window was UTC.
7. **Every reboot is explained.** Each connection reports `reset_reason` (v1: unexplained mid-match reboots).
8. **Forward-compatible.** Unknown fields are stored but never required; unknown event types and state values are accepted as-is; a missing optional field never erases a stored value.

## 3. Identity

- `device_id` = the ESP32-S3 factory MAC from eFuse (`esp_efuse_mac_get_default`), **12 lowercase
  hex digits, no separators**: `f412fa9c012e`. Immutable, unique, never typed by a person.
- **QR sticker payload:** `P2:f412fa9c012e`. The `P2:` prefix lets every scanner tell a 2.0 racket
  from a v1 one (v1 QR = bare serial).
- Human names ("Padel 3") are labels kept by the backend, per event, as in v1.

## 4. Transport — LIVE since 2026-10-22

- **MQTT 5 over WebSocket over TLS**: `wss://mqtt.padelsense.net/mqtt` (port 443, WebSocket
  subprotocol `mqtt`). The hostname is **Cloudflare-proxied**: the server's IP stays hidden and no MQTT
  port is open on it. Path: Cloudflare → Caddy → Mosquitto (`127.0.0.1:9001`, websockets listener).
- Server certificate verified with ESP-IDF's **certificate bundle** (`esp_crt_bundle_attach`), never
  insecure mode. The bundle covers whichever CA Cloudflare's edge uses.
- Client id = `device_id` (both while enrolling and afterwards).
- **Racket login:** `clean_start = false`, **session expiry 3600 s**, so QoS 1 commands queued during a short
  drop are still delivered — *unless they expired* (MQTT 5 message expiry).
- **Enrolment login:** `clean_start = true`, **session expiry 0** — it must leave no session behind for
  the racket's real login (found in testing: the enrolment session otherwise carries over).
- Keepalive **30 s** (Cloudflare closes idle WebSockets at ~100 s). Last will on `…/state`:
  `{"online": false}`, QoS 1, retained — the broker publishes it ~45 s after the racket vanishes.
- Cloudflare occasionally resets WebSockets; the racket just reconnects (esp-mqtt does it with backoff).

## 5. Enrolment (once per racket) — ASSUMPTION: field enrolment

A factory-fresh racket holds only the shared **enrolment credential** (username `enrol`). Connected with
client id = its `device_id`, it may **write** `perceptio/enrol/<device_id>/request` and **read**
`perceptio/enrol/<device_id>/response` — nothing else. (Enrolment lives outside `perceptio/2/` on
purpose: under the per-racket rule `perceptio/2/%u/#` the shared `enrol` user would otherwise own
`perceptio/2/enrol/#` and read other rackets' replies, which carry their passwords.)

```
racket                                broker / backend                         staff
  │ CONNECT user=enrol client=<id>           │                                    │
  │ SUB perceptio/enrol/<id>/response        │                                    │
  │ PUB perceptio/enrol/<id>/request ──────► │ racket listed as PENDING           │
  │   {device_id, model, hw_rev, fw,         │ in the 2.0 dashboard  ───────────► │ scans the QR,
  │    caps, nonce}                          │                                    │ checks the id, Approve
  │                                          │ ◄───────────────────────────────── │
  │ ◄──── perceptio/enrol/<id>/response      │ creates user <id> with a random    │
  │   {status:"approved", username:"<id>",   │ 32-byte password, ACL = own topics │
  │    password:"…", ws_url:"…", nonce}      │                                    │
  │ store in encrypted NVS, DISCONNECT       │                                    │
  │ CONNECT user=<id> password=…  ─────────► │ normal operation (§6)              │
```

- `nonce` (random, generated by the racket) is echoed back so a racket only accepts the response to
  its own request.
- The response is published **once, not retained**, and only after a person approved.
- Rejected / revoked: `{"status":"rejected"}`; a revoked racket's broker user is deleted and it falls
  back to the enrolment credential (shows as pending again).
- Per-racket broker ACL (Mosquitto pattern): `pattern readwrite perceptio/2/%u/#` — a racket can
  never read or write another racket's topics.

## 6. Topics

All under `perceptio/2/<device_id>/`.

| Topic | Direction | QoS | Retained | Purpose |
|---|---|---|---|---|
| `hello` | racket → backend | 1 | yes | announcement, on every (re)connect (§7) |
| `state` | racket → backend | 1 | yes | current state on every change; last will = offline |
| `telemetry` | racket → backend | 0 | no | every 10 s while connected |
| `event` | racket → backend | 1 | no | discrete events, replayed after an outage (§10) |
| `cmd` | backend → racket | 1 | no | commands (§8) |
| `ack` | racket → backend | 1 | no | command outcomes (§8) |

Payloads are UTF-8 JSON, one object per message, ≤ 4 KB.

## 7. Announcement — `hello`

Published right after every successful CONNECT, before anything else, **and again (retained) whenever the
inventory changes** — a recording stops, upload progress moves, a recording is deleted — so the broker's retained
copy is never stale (added 2026-10-22: the dashboard showed 1 stored recording after a second one stopped).
The backend also keeps its own recordings register from `rec_started`/`rec_stopped` events, so it is current
even if the racket does not re-announce.

```json
{
  "proto": 2,
  "device_id": "f412fa9c012e",
  "model": "perceptio-2", "hw_rev": "R2", "fw": "3.0.0", "build": "a1b2c3d",
  "boot_id": 1731, "reset_reason": "brownout", "uptime_s": 4210,
  "clock": { "utc_ms": 1792500000123, "synced": true, "offset_ms": -3, "last_sync_utc_ms": 1792499990000 },
  "state": "recording",
  "current_rec": { "rec_id": "f412fa9c012e-1731-7", "rec_tag": "m-24/p-51",
                   "started_utc_ms": 1792496400000, "samples": 3601233, "rate_hz": 1000 },
  "recordings": [
    { "rec_id": "f412fa9c012e-1731-6", "rec_tag": "m-23/p-51", "bytes": 48211000,
      "uploaded_bytes": 0, "complete": true, "started_utc_ms": 1792490000000, "duration_ms": 3540000 }
  ],
  "storage": { "total_mb": 0, "free_mb": 0 },
  "power": { "battery_pct": 64, "charging": false, "plugged": false },
  "net": { "ip": "192.168.1.40", "rssi": -61 },
  "caps": { "gyro": 1, "accel": [ { "fsr_g": 16 }, { "fsr_g": 320 } ], "rate_hz": [1000] },
  "config_hash": "9f2c1a"
}
```

| Field | Required | Notes |
|---|---|---|
| `proto` | yes | this document = 2. Backend refuses unknown major versions with an `event`-visible reason. |
| `boot_id` | yes | monotonically increasing per racket, persisted in NVS; +1 every boot. |
| `reset_reason` | yes | from `esp_reset_reason()`: `poweron`, `sw`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `brownout`, `deepsleep`, `ext`, `unknown`. |
| `clock` | yes | `synced` = SNTP succeeded since boot; `offset_ms` = last measured correction. |
| `state` | yes | same vocabulary as §9. |
| `current_rec` | if recording | lets the backend recognise a recording it did not command (button start). |
| `recordings` | yes (may be empty) | **inventory** of what is stored and not yet fully uploaded. Drives the upload step. |
| `config_hash` | yes | hash of the racket's active settings; mismatch → backend re-sends `configure`. |
| everything else | optional | absent ≠ zero; never overwrites a stored value. |

## 8. Commands — `cmd` / `ack`

```json
{ "id": "c-8812", "cmd": "start_recording", "issued_utc_ms": 1792500000000,
  "expires_utc_ms": 1792500030000, "args": { "rec_tag": "m-24/p-51" } }
```

- `id`: unique, backend-generated. The racket remembers the last 64 ids and **ignores duplicates**
  (re-acks with the original outcome).
- **Expiry, twice:** MQTT 5 *message expiry interval* (broker drops it undelivered) **and**
  `expires_utc_ms` (racket refuses it if its clock is synced and it arrives late → `ack rejected expired`).
  Default expiry: 30 s for `start_recording`/`stop_recording`, 10 min for `configure`.
- Every command gets exactly one terminal ack:

```json
{ "id": "c-8812", "status": "applied", "utc_ms": 1792500000412,
  "result": { "rec_id": "f412fa9c012e-1731-8" } }
```

`status` ∈ `received` (optional, immediate) → `applied` | `rejected`. `rejected` carries `reason`:
`expired`, `busy`, `bad_args`, `storage_full`, `battery_low`, `unsupported`, `not_recording`.

Dashboard states per command: **queued** (racket offline) → **delivered** → **applied / rejected /
expired** (no ack before expiry). Staff always see which one.

Initial command set:

| `cmd` | `args` | Notes |
|---|---|---|
| `start_recording` | `rec_tag` (backend match/player ref, ≤ 64 chars), optional `rate_hz` | result: `rec_id` |
| `stop_recording` | optional `rec_id` (must match the current one) | result: `rec_id`, `samples`, `bytes` |
| `configure` | any subset of settings; **absent = unchanged** (v1 lesson) | result: new `config_hash` |
| `sync_time` | — | forces SNTP now |
| `identify` | `seconds` | blink/buzz so staff can find the racket |
| `reboot` | — | ack `applied` before restarting |
| `upload` | `rec_id, url, token, chunk_bytes` | resumable HTTPS upload, see `data-v2.md` §2; ack `applied` with the server's verification as `result`; `rejected busy` while recording |
| `delete_recording` | `rec_id` | sent by the backend only after it verified the upload |

## 9. State — `state`

Published (retained) on every change:

```json
{ "online": true, "state": "recording", "rec_id": "f412fa9c012e-1731-7", "since_utc_ms": 1792496400000,
  "boot_id": 1731, "seq": 418 }
```

`state` ∈ `idle`, `armed`, `recording`, `stopping`, `uploading`, `charging`, `updating`, `error`, **`off`**.

**Deliberate shutdown (added 2026-10-22, firmware sends it):** before powering down on purpose the racket
publishes a `shutdown` event (§10) and then a retained state with an optional **`reason`**:

```json
{ "online": false, "state": "off", "reason": "user", "since_utc_ms": 1792676311224, "boot_id": 1731, "seq": 420 }
```

`reason` is free text; suggested values: `user`, `low_battery`, `ota`, `overheat`. `online` may be omitted on this
message (the backend treats `off`/`shutdown` as offline). If the racket then dies before a clean DISCONNECT, the
broker's bare last will (`{"online": false}`) arrives later; the backend **keeps** the shutdown record instead
of overwriting it. The dashboard shows **off · reason** for a deliberate shutdown and **offline** for a lost
connection (power cut, out of range, crash).
The last will sets `{"online": false}` — meaning **unreachable**, not broken: a recording racket out of
Wi-Fi range is healthy (the opposite of v1, where silence was the only signal available).

## 10. Events and offline replay — `event`

```json
{ "boot_id": 1731, "seq": 419, "utc_ms": 1792496401000, "mono_ms": 4210331,
  "type": "rec_started", "data": { "rec_id": "f412fa9c012e-1731-7", "by": "button" } }
```

- `(boot_id, seq)` is unique per racket; `seq` restarts at 0 each boot. Backend de-duplicates on it.
- While disconnected the racket appends events to a ring buffer (≥ 256 entries, in PSRAM, mirrored to
  NAND so a reboot does not lose them) and **replays them in order** after the next `hello`.
- `utc_ms` may be `null` if the clock never synced this boot; `mono_ms` (since boot) is always set.
- Recording events carry the recording's identity (as Padel 0 sends them, 2026-10-22):
  `rec_started` → `data: {rec_id, by: "ui"|"button"|"cmd", rec_tag}`;
  `rec_stopped` → `data: {rec_id, by, samples, bytes, duration_ms, dropped, complete}`.
- Initial types: `boot`, `rec_started`, `rec_stopped`, `imu_reinit`, `sensor_fault`, `storage_low`,
  `battery_low`, `wifi_lost`, `wifi_back`, `time_synced`, **`shutdown`** (`data.reason`, same values as the
  state's `reason`), `error` (`data.code`, `data.msg`).
- **Unknown event types are stored as-is, never refused** (the backend has no type allow-list). The
  firmware may add types freely; the dashboard shows them by name.

## 11. Time

- `clock.synced` in `hello` is a snapshot at connect and is often still `false` (SNTP not finished). When SNTP
  completes the racket MUST publish a `time_synced` event (`data.offset_ms`). Unknown values are `null`, never a
  sentinel such as 2147483647. The dashboard measures the real skew from `utc_ms` in telemetry anyway.
- SNTP at boot, then every 10 min while connected, and **always at `start_recording` and
  `stop_recording`** when the network is up.
- Every sample is timestamped from the racket's monotonic microsecond clock. Each recording stores
  **two UTC anchors** (`(mono_us, utc_ms)` at start and at stop) so the backend can convert to UTC and
  measure drift. A recording with no synced anchor is still valid; it is linked by `rec_tag`.

## 12. Telemetry — `telemetry`

Every 10 s while connected, QoS 0, small, last value wins:

```json
{ "utc_ms": 1792500010000, "battery_pct": 64, "charging": false, "rssi": -63, "temp_c": 38.5,
  "free_mb": 6900, "heap_kb": 180, "psram_kb": 1650, "rec_samples": 3611233, "dropped_samples": 0 }
```

`dropped_samples` must be 0; anything else is a logging-path fault worth surfacing.

## 13. Security layers (as built, 2026-10-22)

1. Server IP hidden behind Cloudflare; no MQTT port exposed; Mosquitto bound to localhost.
2. TLS end to end, server certificate verified by the racket (certificate bundle).
3. No anonymous access. Per-racket login = 32 random bytes, created only when a person approves.
4. ACL: a racket reads/writes only `perceptio/2/<its id>/#`; the enrolment login only its own
   request/response. **Verified by delivery tests through Cloudflare** (`tools/acl_test.py`): a racket
   receives only its own topics, the enrolment login only its own reply, and forged commands/replies
   never reach the bus.
5. Broker limits: max packet 16 KB, 20 in-flight, 200 queued per client.
6. Commands expire (broker + racket), are de-duplicated and acknowledged; expired commands are **not**
   delivered on reconnect (verified).
7. Revocation deletes the racket's login at once and clears its retained topics.
8. **To do on the racket:** ESP32-S3 Secure Boot v2 + Flash Encryption + NVS encryption, so a stolen racket
   can neither yield its credential nor run unsigned firmware.
9. **To do on the server:** firewall 80/443 to Cloudflare's IP ranges only; Cloudflare rate limit on `/mqtt`.

## 14. Open items

1. NAND capacity → `storage` numbers, retention policy on the racket.
2. **Bulk upload protocol** (next spec): resumable chunks over HTTPS, keyed by `rec_id`, file header with
   both UTC anchors, `rec_tag`, sensor ranges and sample format; backend confirms before `delete_recording`.
3. OTA for 2.0: with a TLS-capable S3 the racket can pull firmware from the backend (v1 could not).
4. LAN discovery (`_perceptio._tcp` mDNS) for local tools — optional.
5. Server sizing: ~50 MB per racket per match; 66 GB free on this server ≈ 160 racket-matches before
   a retention or storage decision is needed.
