/*
 * Factory reset (0.3.59). Two entrances, one routine, always executed by the main task:
 *
 *   backend command  factory_reset {scope: "pairing"|"all", force}   -> factory_request() from the cloud task, served
 *                    by factory_tick() in the main loop (like app_request_power_off).
 *   button, offline  from OFF (cable out): press and HOLD; the charger wakes the board; when the 3 s WAKE2 flag is seen
 *                    during the boot of a button-woken device the LED blinks white ("armed"); release, then ONE short press
 *                    within 6 s confirms -> scope all. No press -> normal boot, nothing changed. The charger's hardware
 *                    cut (MR_LPRESS) is moved to 10 s so the hold can outlast the boot; holding that long = ship mode,
 *                    nothing changed. The MCU never sees the button level, only WAKE1 (0.3 s) / WAKE2 (3 s) flags, so a
 *                    plain "hold 10 s" cannot be implemented.
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
/* Boot-time gesture check, main task, cheap when nothing is pending. Call at a few points of the boot sequence
 * (≥ 3 s after power-on) and once before the main loop; it blocks ~6 s only when the gesture is armed. */
void      factory_boot_check(void);
