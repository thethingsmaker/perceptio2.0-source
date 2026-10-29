/*
 * Power states and policies (refactor step 3d, fw 0.3.32 — moved out of main.c, behaviour unchanged).
 *
 *   ON           everything running.
 *   OFF          BQ25180 ship mode: SYS disconnected from the cell. Wake: button press or USB power.
 *   CHARGE-IDLE  the user switched off but a cable is in (ship mode cannot engage): a minimal loop shows the charge
 *                state on the LED, no WiFi/UI/sampler. A press turns the device on; pulling the cable -> ship mode.
 * NVS "user_off" carries the user's intent across the reboot into CHARGE-IDLE.
 *
 * Charge gate: there is no NTC, so the board (IMU die) temperature is the ambient proxy: charging pauses outside
 * chg_tmin_c..chg_tmax_c and resumes with 5 K hysteresis; a BQ safety-timer fault restarts the charge cycle.
 *
 * What must happen before switching off (close a recording, tell the backend, confirm a fresh OTA image) is the
 * caller's business: power_mgr calls the `prepare` hook given to power_mgr_init().
 */
#pragma once
#include <stdbool.h>

typedef void  (*power_mgr_prepare_fn)(const char *why);   /* before OFF / CHARGE-IDLE */
typedef float (*power_mgr_temp_fn)(void);                  /* board temperature, °C, readable without the sampler */

void power_mgr_init(power_mgr_prepare_fn prepare, power_mgr_temp_fn board_temp);
void power_mgr_off(const char *why);       /* never returns: prepare -> ship mode, or reboot into CHARGE-IDLE (cable in) */
void power_mgr_tick(void);                 /* main loop: serves app_request_power_off() (UI / cloud), any task */
bool power_mgr_charge_gate(float board_c); /* once per status tick: true = charging paused by temperature */
void power_mgr_charge_idle(void);          /* never returns: the CHARGE-IDLE loop */
/* Early at boot: reports how the last power-off ended (ship mode / deep-sleep fallback) and decides for a device the
 * user had switched off: cable in -> CHARGE-IDLE (never returns); woken by the fallback TIMER with nobody pressing
 * -> back to sleep (never returns); woken by the button -> clears user_off and returns (turn on). */
void power_mgr_boot(bool plugged);
/* Low-battery shutdown (0.3.33), once a second: on battery, when the averaged VBAT stays <= parameter bat_off_mv
 * (default 3300) for 30 s, switch off exactly like the button (recording closed first, backend told) into ship mode.
 * Without it a flat board browns out at the BQ's 3.0 V BUVLO, the unloaded cell recovers, it boots, and repeats. */
void power_mgr_battery_tick(bool plugged);
