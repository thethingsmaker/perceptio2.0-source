#include <stdint.h>
#include "button.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "BUTTON";
#define DOUBLE_PRESS_US (1500 * 1000)            /* second press (its 0.3 s WAKE1) within 1.5 s of the first */

static volatile power_btn_t s_evt;              /* latest raw event from the /INT hook (power.c) */
static button_action_fn     s_double, s_long;
static int64_t              s_first_us;         /* WAKE1 of a possible double press (0 = none) */

static void on_hook(power_btn_t k) { s_evt = k; }

void button_init(button_action_fn on_double, button_action_fn on_long)
{
    s_double = on_double; s_long = on_long;
    power_set_button_hook(on_hook);
}

power_btn_t button_take(void)
{
    power_btn_t k = power_poll_button();         /* poll as well as /INT (the flags are latched in the charger) */
    if (!k) k = s_evt;
    s_evt = 0;
    return k;
}

void button_tick(void)
{
    power_btn_t k = button_take();
    int64_t now = esp_timer_get_time();
    if (k == BTN_LONG) { s_first_us = 0; if (s_long) s_long(); }   /* power-off closes a running session first */
    else if (k == BTN_SHORT) {
        if (s_first_us && now - s_first_us <= DOUBLE_PRESS_US) {
            s_first_us = 0;
            ESP_LOGI(TAG, "double press");
            if (s_double) s_double();
        } else s_first_us = now;
    }
    if (s_first_us && now - s_first_us > DOUBLE_PRESS_US) { s_first_us = 0; ESP_LOGI(TAG, "single press (ignored)"); }
}
