/*
 * Network watch + fallback access point (refactor step 3b, fw 0.3.29 — moved out of main.c, behaviour unchanged).
 *
 * The UI must always be reachable. The AP "Perceptio-<device id>" (password NETWATCH_AP_PASSWORD) is started
 *   - at boot when no saved network joins (main calls netwatch_start_ap), and
 *   - by netwatch_tick_1s() when the backend has been unreachable for NETWATCH_AP_AFTER_S (joined a network without
 *     internet, DNS dead, router gone...).
 * It is switched off again once the backend has been reachable for 30 s (retried every 30 s while a phone uses it).
 * "Reachable" is decided by the caller's backend_ok() (main: the cloud link answered), so this module does not depend
 * on the cloud module.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NETWATCH_AP_AFTER_S   90
#define NETWATCH_AP_PASSWORD  "perceptio"           /* same on every unit: development only */

typedef bool (*netwatch_ok_fn)(void);               /* backend reachable right now? */
typedef void (*netwatch_ap_up_fn)(void);            /* the AP just came up (main: make sure the web UI runs) */

void      netwatch_init(netwatch_ok_fn backend_ok, netwatch_ap_up_fn on_ap_up);
esp_err_t netwatch_start_ap(const char *why);       /* no-op when the AP is already up */
void      netwatch_tick_1s(void);                   /* call once a second */
uint32_t  netwatch_backend_bad_s(void);             /* seconds the backend has been unreachable (LED policy) */
