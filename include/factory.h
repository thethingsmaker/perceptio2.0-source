/*
 * Factory reset (0.3.59). Two entrances, one routine, always executed by the main task:
 *
 *   backend command  factory_reset {scope: "pairing"|"all", force}   -> factory_request() from the cloud task, served
 *                    by factory_tick() in the main loop (like app_request_power_off).
 *   button, offline  from OFF (cable out): switch on with the button; when the LED turns blue (~2.5 s) press and HOLD
 *                    again; at 3 s (WAKE2) the LED blinks white ("armed"); release, then ONE short press within 6 s
 *                    confirms -> scope all. No press -> normal boot, nothing changed. The MCU never sees the button
 *                    level, only the charger's WAKE1 (0.3 s) / WAKE2 (3 s) flags, and the charger sets none for the
 *                    press that woke it from ship mode — hence the second press. Its hardware long-press cut
 *                    (MR_LPRESS) is 10 s: holding that long = ship mode, nothing changed.
 *
 *   scope pairing: BLE bond erased, pairing window reopened, phone dropped. No reboot.
 *   scope all:     + saved WiFi networks, parameters back to defaults, every recording on the NAND. Keeps device_id,
 *                  cloud enrolment, OTA token, firmware. Event `factory_reset`, backend told (`cloud_shutdown`), reboot.
 *   force: a running recording is closed first; without it the request is refused while recording/uploading/updating.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum { FACTORY_PAIRING = 0, FACTORY_ALL = 1 } factory_scope_t;

/* Any task. ESP_ERR_INVALID_STATE: recording/uploading/updating and !force; ESP_OK: queued for the main loop. */
esp_err_t factory_request(factory_scope_t scope, bool force, const char *by, const char *cmd_id);
void      factory_tick(void);                 /* main loop: serves a queued request (does not return for scope all) */
/* Boot-time gesture check, main task, once, right after the LED turns blue: waits ≤ 1.2 s for a press on a button-woken
 * boot (nothing on other boots), longer only while a press is in progress; blocks ~6 s more when armed. */
void      factory_boot_check(void);
