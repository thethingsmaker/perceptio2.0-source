/*
 * Status LED policy (refactor step 3c, fw 0.3.31 — moved out of main.c, behaviour unchanged).
 *
 * One place decides the LED, by priority (colours/patterns in led.h, meanings in README "LED"):
 *   identify (white blink, own timer — never overridden here) > self-test failed (red blink, 3 s after boot)
 *   > recording (red breathing) > charge fault (yellow blink) > no WiFi / no backend / fallback AP (blue blink)
 *   > charged (yellow solid) > charging (yellow breathing) > all good (green breathing).
 * Charge states are shown only with the cable in AND a battery present. "Charge fault" = paused by the temperature
 * gate, BQ safety timer, or cable + battery but neither charging nor done for 10 s.
 *
 * The module knows nothing about where the inputs come from: main fills led_inputs_t from the charger, the store,
 * netwatch and the self-test, once a second.
 */
#pragma once
#include <stdbool.h>
#include "led.h"

typedef struct {
    bool boot_fail;        /* self-test failed, inside its 3 s display window */
    bool recording;
    bool plugged;          /* VIN power good */
    bool battery;          /* a cell is present (power_battery_present) */
    bool charging;         /* CC or CV */
    bool done;             /* charge completed */
    bool safety_fault;     /* BQ safety timer expired */
    bool charge_paused;    /* temperature gate */
    bool net_bad;          /* fallback AP up, or backend unreachable for >= 20 s */
} led_inputs_t;

led_mode_t led_policy_decide(const led_inputs_t *in);   /* call once a second: it counts the 10 s "not charging" */
void       led_policy_apply(const led_inputs_t *in);    /* decide + led_set() when it changed; leaves identify alone */
