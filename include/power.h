/* Battery voltage (ADC1_CH3 behind a 1 M/1 M divider, eFuse-calibrated) and the BQ25180 /INT line
 * (button press/release and charger events). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bq25180.h"

typedef enum { BTN_SHORT = 1, BTN_LONG = 2 } power_btn_t;
typedef void (*power_button_fn)(power_btn_t kind);   /* task context; SHORT = WAKE1 (≥300 ms), LONG = WAKE2 (≥3 s) */

/* Last resort when ship mode does not engage: deep sleep with wake on the button (/INT low) and a timer.
 * Never returns. */
void      power_deep_sleep_fallback(uint32_t timer_s);

esp_err_t power_init(void);
float     power_vbat(void);                 /* volts, calibrated when eFuse data is present */
/* State of charge 0..100 % estimated from terminal voltage (LiPo OCV table). No fuel gauge on the board:
 * reads high while charging, low under heavy load; good to ~10 %. */
int       power_soc_pct(float vbat, bool charging);
bool      power_vbat_calibrated(void);
esp_err_t power_charger_status(bq25180_status_t *st);
/* /INT edge counter (diagnostics) and optional callback */
uint32_t  power_int_edges(void);
void      power_set_button_hook(power_button_fn fn);
/* Poll path (independent of /INT): consume latched WAKE flags, return 0 / BTN_SHORT / BTN_LONG. */
power_btn_t power_poll_button(void);
/* Implemented in main.c: ask the main loop to switch the device off (same path as the 5 s button press: closes a
 * running session, tells the backend, then ship mode — or a reboot into CHARGE-IDLE when a cable is in). */
void      app_request_power_off(const char *why);
