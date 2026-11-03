/*
 * BLE (NimBLE peripheral) — fw 0.3.44: measurement build, advertising only.
 *
 * Advertises "Perceptio-<device id>" (connectable, 500 ms interval) so a phone app can find the racket; no GATT
 * service of our own yet (only GAP/GATT). Logs what it costs (internal RAM before/after) and every connect/disconnect.
 * Next steps (docs: CLAUDE.md): API tunnel service, pairing once out of the box, live stream.
 * Controller and host run on core 0 (core split), host memory in PSRAM (sdkconfig.defaults).
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t ble_init(const char *device_id);   /* after WiFi/cloud init; starts advertising once the host syncs */
bool      ble_connected(void);
