/*
 * Backend watch (refactor step 3b, fw 0.3.29; the fallback access point it used to start was removed in 0.3.46 —
 * phone setup moves to BLE).
 *
 * Counts how long the backend has been unreachable, for the LED policy ("no network / no backend": blue slow blink
 * after 20 s). "Reachable" is decided by the caller's backend_ok() (main: the cloud link answered), so this module
 * does not depend on the cloud module. Rejoining the saved networks is net.c's job.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef bool (*netwatch_ok_fn)(void);               /* backend reachable right now? */

void      netwatch_init(netwatch_ok_fn backend_ok);
void      netwatch_tick_1s(void);                   /* call once a second */
uint32_t  netwatch_backend_bad_s(void);             /* seconds the backend has been unreachable (LED policy) */
