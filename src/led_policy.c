#include <stdint.h>
#include "led_policy.h"
#include "esp_log.h"

static const char *TAG = "LED";

led_mode_t led_policy_decide(const led_inputs_t *in)
{
    static uint32_t notchg_s;                       /* cable + cell but neither charging nor done, seconds */
    notchg_s = (in->plugged && in->battery && !in->charging && !in->done) ? notchg_s + 1 : 0;
    bool chg_fault = in->plugged && in->battery && (in->charge_paused || in->safety_fault || notchg_s >= 10);
    if (in->boot_fail)                           return LED_BOOT_FAIL;
    if (in->recording)                           return LED_RECORDING;
    if (chg_fault)                               return LED_CHARGE_FAULT;
    if (in->ap_up)                               return LED_NET;          /* fast blink: connect a phone */
    if (in->net_bad)                             return LED_NET_SEARCH;   /* slow blink: no AP yet */
    if (in->plugged && in->battery && in->done)  return LED_FULL;
    if (in->plugged && in->battery && in->charging) return LED_CHARGING;
    return LED_OK;
}

void led_policy_apply(const led_inputs_t *in)
{
    if (led_get() == LED_IDENTIFY) return;          /* "find me" / refused-start flash runs its own timer */
    led_mode_t m = led_policy_decide(in);
    static led_mode_t last = LED_OFF;
    if (m != led_get()) led_set(m);
    if (m != last) {                                /* one line per change: lets a log show what the LED showed */
        static const char *const N[] = {
            [LED_OFF] = "off", [LED_BOOT] = "booting", [LED_OK] = "ok (green)", [LED_BOOT_FAIL] = "self-test failed",
            [LED_NET] = "fallback AP up (blue fast blink)", [LED_RECORDING] = "recording", [LED_CHARGING] = "charging",
            [LED_CHARGE_FAULT] = "charge fault", [LED_FULL] = "charged", [LED_IDENTIFY] = "identify",
            [LED_NET_SEARCH] = "no network/backend, no AP yet (blue slow blink)",
        };
        ESP_LOGI(TAG, "%s", (unsigned)m < sizeof N / sizeof N[0] && N[m] ? N[m] : "?");
        last = m;
    }
}
