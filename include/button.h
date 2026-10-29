/*
 * Button gestures (refactor step 3a, fw 0.3.28 — moved out of main.c, behaviour unchanged).
 *
 * The button is the BQ25180 push-button: the charger reports WAKE1 after 0.3 s of hold (BTN_SHORT) and WAKE2 after 3 s
 * (BTN_LONG); there is no release event. Two gestures only:
 *   two presses within 1.5 s  -> on_double   (start/stop recording)
 *   hold 3 s                  -> on_long     (power off)
 * A single press does nothing: a long press always begins with a WAKE1, which once started a recording on the way to off.
 *
 * Raw events arrive from power.c (/INT hook) and from the poll path; button_tick() turns them into gestures in the
 * caller's task (main loop, every 100 ms), so the actions run there, never in an ISR or the charger task.
 */
#pragma once
#include "power.h"

typedef void (*button_action_fn)(void);

void        button_init(button_action_fn on_double, button_action_fn on_long);   /* registers the power.c hook */
void        button_tick(void);              /* gesture recognition: call every 100 ms */
power_btn_t button_take(void);              /* raw pending event (poll + hook), cleared: for loops without gestures */
