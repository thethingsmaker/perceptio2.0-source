/*
 * BLE (NimBLE peripheral) — the phone's way into the racket (BLE line, fw 0.3.44+).
 *
 * Advertises "Perceptio-<device id>" (connectable, 500 ms). One phone at a time.
 *
 * API tunnel (0.3.53, docs/ble-v1.md): one service, the same /api/v1 the web page uses, carried as JSON:
 *   REQ  (write, no response)   phone -> racket   {"id":7,"m":"GET","p":"/api/v1/status","b":{...}}
 *   RSP  (notify)               racket -> phone   {"id":7,"s":200,"b":{...}}
 * Every message is split into chunks of (MTU - 3 - 1) bytes; the first byte of each chunk is a frame byte:
 * bit 7 = last chunk of the message, bits 6..0 = chunk sequence number (0, 1, 2 … per message). Messages up to
 * BLE_MSG_MAX bytes. One request at a time per connection; a second one while the first runs is answered 429.
 * A request is served by an HTTP exchange with the board's own web server over loopback (127.0.0.1:80) in the BLE API
 * task, so the two transports can never drift apart: no handler is duplicated. Binary answers (core dump, recording
 * files) are not carried: 415.
 * Security (0.3.55): LE Secure Connections, Just Works, bonding, ONE bond. Out of the box the racket accepts a pairing
 * from any phone during the first BLE_PAIR_WINDOW_S after power-on (the unboxing window); once a bond exists only that
 * phone may stay connected (any other is disconnected at once) and the window never reopens — a factory reset (or the
 * `POST /api/v1/ble/unpair` hook, which reopens the window) is the only way to pair another phone. REQ writes need an
 * encrypted link; the racket starts encryption/pairing itself right after the connection. Pairing is refused while
 * recording (the bond is written to NVS). Subscriptions are not persisted (no NVS write on subscribe).
 * Controller and host run on core 0 (core split), host memory in PSRAM (sdkconfig.defaults).
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#define BLE_MSG_MAX 12288                 /* request and response buffers (PSRAM) */

#define BLE_PAIR_WINDOW_S 600             /* unboxing window: pairing accepted this long after power-on while unpaired */

typedef struct { bool bonded, pairing_open, connected, encrypted; int window_s; } ble_status_t;

esp_err_t ble_init(const char *device_id);   /* after the web server is up; starts advertising once the host syncs */
bool      ble_connected(void);
void      ble_status(ble_status_t *st);
esp_err_t ble_unpair(void);                  /* erase the bond(s), reopen the window, drop the phone; ESP_ERR_INVALID_STATE while recording */
