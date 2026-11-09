/*
 * Idle auto power-off (fw 0.3.69): a racket left lying still switches itself off after parameter `idle_off_min`
 * minutes (Power group, default 20, 0 = never), exactly like the 3 s button hold (session closed, backend told,
 * ship mode — or CHARGE-IDLE with the cable in).
 *
 * "Still" is judged once a second from the sampler's last sample (the IMU runs all the time, no extra reads).
 * The gyro is the signal: handling a racket always rotates it, a vibrating table (washing machine, car boot, gym
 * floor) shakes it without rotating it, and the gyro's noise + bias stay far below IDLE_GYRO_DPS. The accelerometer
 * only catches a reposition between two samples (gravity vector changed by more than IDLE_ACC_MG on an axis).
 * One "moving" second is ignored (a knock, a vibration spike, a torn read — core 1 writes the sample without a lock);
 * two in a row restart the count. A racket carried in a bag never qualifies, one on a table or in a locker does.
 *
 * The count is held at zero while the racket is in use, so it can never fire mid-way through anything:
 * recording, a phone subscribed to LIVE, a tunnel request other than the app's background polls (status/ota) in the
 * last IDLE_REQ_GRACE_S, upload, OTA, a WiFi test, cable in (nothing to save, and a bench board stays reachable),
 * IMU not delivering (no data = no verdict). The last IDLE_WARN_S the LED blinks amber (LED_IDLE_WARN).
 * Once a minute while counting: `IDLE: still N min (gyro x.x dps, da y mg)` — the margins against the thresholds.
 */
#pragma once
#include <stdbool.h>

#define IDLE_GYRO_DPS     5.0f     /* |gyro| at/above this = being handled (noise ~0.2 dps, bias < 1 dps) */
#define IDLE_ACC_MG       150.0f   /* gravity vector moved by this on an axis since the previous second = repositioned */
#define IDLE_MOVE_S       2        /* consecutive moving seconds that restart the count (1 = a knock / vibration spike) */
#define IDLE_WARN_S       30       /* amber blink before the switch-off */
#define IDLE_REQ_GRACE_S  120      /* a tunnel request counts as activity for this long */

void idle_tick_1s(void);           /* main loop, once a second */
bool idle_warning(void);           /* within IDLE_WARN_S of switching off (LED) */
