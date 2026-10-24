/* Status LED patterns — always breathing (continuous feedback), never blinking. One 25 Hz task on core 0. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_BOOT,          /* blue, 1 s */
    LED_OK,            /* green, 3 s */
    LED_FAULT,         /* red, 1 s */
    LED_CHARGING,      /* amber, 2.5 s */
    LED_FULL,          /* dim green, 6 s */
    LED_CHARGE_PAUSED, /* red, 1.5 s */
    LED_RECORDING,     /* cyan, 2 s */
    LED_IDENTIFY,      /* white, fast 0.6 s — "find me" */
} led_mode_t;

esp_err_t led_start(void);               /* after lp5810_init() */
void      led_set(led_mode_t mode);      /* takes effect within one tick (40 ms) */
led_mode_t led_get(void);
void      led_pause(bool on);            /* true: task stops driving the LED (selftest colour cycle) */
void      led_identify(uint32_t seconds); /* white flash for N s, then back to the previous mode */
