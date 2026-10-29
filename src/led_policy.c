#include <stdint.h>
#include "led_policy.h"

led_mode_t led_policy_decide(const led_inputs_t *in)
{
    static uint32_t notchg_s;                       /* cable + cell but neither charging nor done, seconds */
    notchg_s = (in->plugged && in->battery && !in->charging && !in->done) ? notchg_s + 1 : 0;
    bool chg_fault = in->plugged && in->battery && (in->charge_paused || in->safety_fault || notchg_s >= 10);
    if (in->boot_fail)                           return LED_BOOT_FAIL;
    if (in->recording)                           return LED_RECORDING;
    if (chg_fault)                               return LED_CHARGE_FAULT;
    if (in->net_bad)                             return LED_NET;
    if (in->plugged && in->battery && in->done)  return LED_FULL;
    if (in->plugged && in->battery && in->charging) return LED_CHARGING;
    return LED_OK;
}

void led_policy_apply(const led_inputs_t *in)
{
    if (led_get() == LED_IDENTIFY) return;          /* "find me" / refused-start flash runs its own timer */
    led_mode_t m = led_policy_decide(in);
    if (m != led_get()) led_set(m);
}
