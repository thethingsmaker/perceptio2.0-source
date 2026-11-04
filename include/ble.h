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
 * Security: none yet — pairing/bonding comes in the next version; until then lab use only.
 * Controller and host run on core 0 (core split), host memory in PSRAM (sdkconfig.defaults).
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#define BLE_MSG_MAX 12288                 /* request and response buffers (PSRAM) */

esp_err_t ble_init(const char *device_id);   /* after the web server is up; starts advertising once the host syncs */
bool      ble_connected(void);
