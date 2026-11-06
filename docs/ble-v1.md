# Perceptio 2.0 — BLE API v1 (fw 0.3.53+)

The phone talks to the racket over Bluetooth Low Energy with **the same JSON API the web page uses** (`API.md`,
`/api/v1/...`). The racket carries each request to its own web server over loopback, so every endpoint, status code
and JSON body is identical on both transports. Nothing is duplicated in the firmware.

## Finding and connecting

- Advertising name: `Perceptio-<device id>` (12 hex digits, e.g. `Perceptio-d40592485f3c`), connectable, every 500 ms.
  The scan response carries the service UUID below.
- One phone at a time. Ask for the biggest ATT MTU (Android: `requestMtu(517)`); the racket accepts up to 512.
- Security (0.3.55): LE Secure Connections, Just Works, bonding, **one bond**. Right after the connection the racket
  starts encryption itself: with a bond it just encrypts; without one it pairs (Android shows the pairing dialog) — but
  only in the **unboxing window**, the first 10 min after power-on while no phone is paired. Outside the window an
  unpaired racket disconnects at once; a paired racket disconnects any phone other than the paired one. The app should
  show "this racket is paired with another phone — factory-reset it to pair" on an immediate disconnect. The REQ
  characteristic requires an encrypted link (writes before encryption fail with insufficient encryption).
  `GET /api/v1/ble` → `{bonded, pairing_open, window_s, connected, encrypted}`; `POST /api/v1/ble/unpair` erases the
  bond, reopens the window and drops the phone ~0.5 s after answering (409 while recording). The phone "forgetting" the
  racket is accepted: the same phone re-pairs and replaces its bond (not while recording).

## GATT

| | UUID | Properties |
|---|---|---|
| Service | `7e2c0001-b8f2-4a3c-9f5e-2d7a1c0e6b10` | primary |
| REQ | `7e2c0002-b8f2-4a3c-9f5e-2d7a1c0e6b10` | write without response (also write) — phone → racket |
| RSP | `7e2c0003-b8f2-4a3c-9f5e-2d7a1c0e6b10` | notify — racket → phone (enable via the CCCD) |
| LIVE | `7e2c0004-b8f2-4a3c-9f5e-2d7a1c0e6b10` | notify — live IMU samples while subscribed (0.3.65), see below |

## Framing (both directions)

A message is UTF-8 JSON, up to **12 288 bytes**. It is sent as chunks of at most `MTU − 4` bytes of payload, each
preceded by one **frame byte**:

```
bit 7      1 = last chunk of this message
bits 6..0  chunk sequence number: 0, 1, 2, … (mod 128), restarting at 0 for every message
```

The receiver concatenates payloads in order; a sequence gap discards the message. The phone must send the chunks of
one message back to back (wait for Android's write callback between chunks) and must not start a second request
before the response to the first has arrived: the racket serves one request at a time and drops any other.

## Messages

Request (REQ):
```json
{"id": 7, "m": "GET", "p": "/api/v1/status"}
{"id": 8, "m": "PATCH", "p": "/api/v1/config", "b": {"ws_hz": 30}}
```
- `id` any integer the phone chooses; it comes back in the response.
- `m` HTTP method as the API documents it; `p` the path (`/api/v1/...`); `b` the JSON body, when the endpoint takes one.

Response (RSP):
```json
{"id": 7, "s": 200, "b": { ...the endpoint's JSON... }}
{"id": 8, "s": 409, "b": {"error": "recording in progress"}}
```
- `s` is the HTTP status the endpoint produced. Tunnel-level errors use the same shape:
  `400` malformed request, `415` the endpoint answered something that is not JSON (core dump, recording files — not
  carried over BLE), `502` the web server did not answer, `507` the response does not fit in 12 288 bytes.

Not available over BLE: `GET /api/v1/recordings/<id>` (the PCR2 file), `GET /api/v1/coredump`, the WebSocket live stream
(`/ws`) — a LIVE characteristic with the same binary frames comes in a later version.

## LIVE stream (0.3.65)

Subscribe to LIVE (CCCD) and the racket notifies one self-contained frame every 50 ms, little-endian:

```
uint32  seq           absolute index of the first sample in this frame
uint16  n             samples in this frame (≤ 25)
uint8   div           decimation: every div-th sample of the IMU rate (4 at 960 Hz → 240 Hz)
uint8   sample_bytes  20
n ×  int16[10]        TEMP, GX, GY, GZ, AX, AY, AZ, HGX, HGY, HGZ   raw LSB — same layout and scaling as /ws (API.md)
```

Gap detection: the next frame's `seq` should be `seq + n*div`. The frame fits one notification at MTU ≥ 48; at the
default 23 nothing is sent. ≈ 12 samples / 248 B per frame at 960 Hz, 5 KB/s. Unsubscribe to stop; the stream also stops
on disconnect. The REQ/RSP tunnel keeps working while LIVE is on (the two share the link; a status poll adds ~1 frame
of latency).
