/*
 * Idle auto power-off (fw 0.3.69): a racket left lying still switches itself off after parameter `idle_off_min`
 * minutes (Power group, default 20, 0 = never), exactly like the 3 s button hold (session closed, backend told,
 * ship mode — or CHARGE-IDLE with the cable in).
 *
 * "Still" is judged once a second from the sampler's last sample (the IMU runs all the time, no extra reads): no
 * rotation (|gyro| < IDLE_GYRO_DPS) and the accelerometer reading 1 g without changing orientation since the second
 * before (IDLE_ACC_MG). One moving second resets the count: a racket carried in a bag never qualifies, one on a table
 * or in a locker does. A torn read (core 1 writes the sample without a lock) can only look like motion = safe side.
 *
 * The count is held at zero while the racket is in use, so it can never fire mid-way through anything:
 * recording, a phone subscribed to LIVE, a tunnel request in the last IDLE_REQ_GRACE_S, upload, OTA, a WiFi test,
 * cable in (nothing to save, and a bench board stays reachable), IMU not delivering (no data = no verdict).
 * The last IDLE_WARN_S the LED blinks amber (LED_IDLE_WARN); any movement cancels it.
 */
#pragma once
#include <stdbool.h>

#define IDLE_GYRO_DPS     5.0f     /* |gyro| below this = not rotating */
#define IDLE_ACC_MG       60.0f    /* |a| within this of 1 g and each axis within this of the previous second */
#define IDLE_WARN_S       30       /* amber blink before the switch-off */
#define IDLE_REQ_GRACE_S  120      /* a tunnel request counts as activity for this long */

void idle_tick_1s(void);           /* main loop, once a second */
bool idle_warning(void);           /* within IDLE_WARN_S of switching off (LED) */
