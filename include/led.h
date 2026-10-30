/* Status LED — one 25 Hz task on core 0. Patterns: breathing, fast blink (4 Hz), solid.
 * The meaning of each colour (README "LED") is decided in led_policy.c (since 0.3.31), by priority:
 *   identify (white blink) > self-test failed (red blink, 3 s after boot) > recording (red breathing)
 *   > charge fault (yellow blink) > fallback AP up (blue fast blink) > no WiFi / no backend, no AP yet (blue slow blink)
 *   > charged (yellow solid) > charging (yellow breathing) > all good (green breathing). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_BOOT,          /* blue breathing, 1 s — booting (self-test, WiFi join) */
    LED_OK,            /* green breathing, 3 s — WiFi + backend reachable, idle */
    LED_BOOT_FAIL,     /* red fast blink — self-test failed (shown 3 s) */
    LED_NET,           /* blue fast blink — fallback AP up: connect a phone now */
    LED_RECORDING,     /* red breathing, 2 s */
    LED_CHARGING,      /* yellow breathing, 2.5 s */
    LED_CHARGE_FAULT,  /* yellow fast blink — paused (temperature), safety timer, charger stopped */
    LED_FULL,          /* yellow solid — charge done */
    LED_IDENTIFY,      /* white fast blink — "find me" */
    LED_NET_SEARCH,    /* blue slow blink, 1 Hz — WiFi or backend down >= 20 s, no AP yet (0.3.35) */
} led_mode_t;

esp_err_t led_start(void);               /* after lp5810_init() */
void      led_set(led_mode_t mode);      /* takes effect within one tick (40 ms) */
led_mode_t led_get(void);
void      led_pause(bool on);            /* true: task stops driving the LED (selftest colour cycle) */
void      led_identify(uint32_t seconds); /* white flash for N s, then back to the previous mode */
