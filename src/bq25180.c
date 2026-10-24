#include "bq25180.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BQ25180";

esp_err_t bq25180_read(uint8_t reg, uint8_t *v)
{ return i2c_master_write_read_device(I2C_PORT, BQ25180_ADDR, &reg, 1, v, 1, pdMS_TO_TICKS(200)); }

esp_err_t bq25180_write(uint8_t reg, uint8_t v)
{ uint8_t b[2] = { reg, v }; return i2c_master_write_to_device(I2C_PORT, BQ25180_ADDR, b, 2, pdMS_TO_TICKS(200)); }

esp_err_t bq25180_set_ichg_ma(uint16_t ma)
{
    uint8_t code;
    if (ma < 5) ma = 5;
    if (ma > 1000) ma = 1000;
    if (ma <= 35) code = ma - 5;                /* 5..35 mA in 1 mA steps */
    else          code = 31 + (ma - 40) / 10;   /* 40..1000 mA in 10 mA steps; 500 mA -> 0x4D */
    uint8_t cur = 0; bq25180_read(BQ25180_ICHG_CTRL, &cur);
    return bq25180_write(BQ25180_ICHG_CTRL, (cur & 0x80) | (code & 0x7F));
}

esp_err_t bq25180_charge_enable(bool en)
{
    uint8_t cur = 0; esp_err_t e = bq25180_read(BQ25180_ICHG_CTRL, &cur);
    if (e != ESP_OK) return e;
    return bq25180_write(BQ25180_ICHG_CTRL, en ? (cur & 0x7F) : (cur | 0x80));   /* bit7 CHG_DIS */
}

esp_err_t bq25180_set_ilim_ma(uint16_t ma)
{
    static const uint16_t steps[8] = { 50, 100, 200, 300, 400, 500, 700, 1100 };
    uint8_t code = 5;
    for (uint8_t i = 0; i < 8; i++) if (steps[i] <= ma) code = i;
    uint8_t tl = 0; esp_err_t e = bq25180_read(BQ25180_TMR_ILIM, &tl);
    if (e != ESP_OK) return e;
    return bq25180_write(BQ25180_TMR_ILIM, (tl & ~0x07) | code);
}

esp_err_t bq25180_restart_charge(void)
{
    esp_err_t e = bq25180_charge_enable(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    e |= bq25180_charge_enable(true);
    ESP_LOGW(TAG, "charge restarted (CE toggled)");
    return e;
}

esp_err_t bq25180_init(void)
{
    uint8_t id = 0;
    esp_err_t e = bq25180_read(BQ25180_MASK_ID, &id);
    if (e != ESP_OK) { ESP_LOGE(TAG, "not responding (%s)", esp_err_to_name(e)); return e; }
    if ((id & 0x0F) != 0) ESP_LOGW(TAG, "unexpected device id %X", id & 0x0F);

    /* I2C watchdog (IC_CTRL bits[1:0]): default 160 s resets every register to defaults when no I2C
     * traffic — which would silently drop ICHG back to 10 mA during a session. Disable (11). */
    uint8_t ic = 0; bq25180_read(BQ25180_IC_CTRL, &ic);
    ic = (ic & ~0x03) | 0x03;                   /* watchdog off */
    ic &= ~0x80;                                /* TS_EN = 0: no NTC on this board; temperature gate is in firmware */
    ic |= 0x0C;                                 /* SAFETY_TIMER[3:2] = 11: disabled. Default 6 h expired on a board that is powered
                                                 * from USB all day (the charger only gets what the 500 mA input leaves) and
                                                 * charging silently stopped. Termination + the firmware thermal gate protect the cell. */
    bq25180_write(BQ25180_IC_CTRL, ic);

    /* Charge profile written as absolute values, never trusted from the chip. 2026-10-23: the register dump read
     * VBAT_CTRL = 0x01 (VBATREG 3.51 V!) with CHARGECTRL0/1 = 0x01 as well — not defaults, never written by firmware.
     * A 3.51 V target shows as "charging (CV)" with zero current while the cell sits at 3.78 V and discharges. */
    static const struct { uint8_t reg, val; const char *name; } prof[] = {
        { BQ25180_VBAT_CTRL,   0x46, "VBAT_CTRL" },    /* VBATREG = 3.50 + 0x46*0.01 = 4.20 V */
        { BQ25180_CHARGECTRL0, 0x2C, "CHARGECTRL0" },  /* defaults: IPRECHG 2x ITERM, ITERM 10 %, VINDPM 4.5 V, THERM 100 C */
        { BQ25180_CHARGECTRL1, 0x56, "CHARGECTRL1" },  /* defaults: IBAT_OCP 1.5 A, BUVLO 3.0 V, interrupts on */
    };
    for (unsigned i = 0; i < sizeof prof / sizeof prof[0]; i++) {
        uint8_t was = 0, now = 0; bq25180_read(prof[i].reg, &was);
        if (was != prof[i].val) {
            bq25180_write(prof[i].reg, prof[i].val); bq25180_read(prof[i].reg, &now);
            ESP_LOGW(TAG, "%s was %02X -> %02X (read back %02X)", prof[i].name, was, prof[i].val, now);
        }
    }

    bq25180_set_ichg_ma(300);                   /* overridden from g_cfg.ichg_ma by main; linear charger -> keep heat down */

    /* TMR_ILIM[7:6] MR_LPRESS: 00 = 5 s (default 01 = 10 s). Long press -> PB_LPRESS_ACTION (ship mode).
     * The WAKE2 flag arrives at ~2-3 s so firmware has time to close a session before the 5 s cut. */
    uint8_t tl = 0; bq25180_read(BQ25180_TMR_ILIM, &tl);
    bq25180_write(BQ25180_TMR_ILIM, tl & 0x3F);

    /* SHIP_RST written as an absolute value: EN_RST_SHIP = 00 (this CANCELS any ship/shutdown request still
     * pending from a previous long press while VIN was present — re-issuing it blocked the push button),
     * PB_LPRESS_ACTION = 10 (long press -> ship mode), WAKE1_TMR = 0 (300 ms), WAKE2_TMR = 1 (3 s), EN_PUSH = 1. */
    uint8_t sr = 0; bq25180_read(BQ25180_SHIP_RST, &sr);
    bq25180_write(BQ25180_SHIP_RST, 0x13);
    uint8_t sr2 = 0; bq25180_read(BQ25180_SHIP_RST, &sr2);
    ESP_LOGI(TAG, "SHIP_RST %02X -> %02X%s", sr, sr2, (sr & 0x60) ? "  (cancelled a pending ship/shutdown request)" : "");

    /* MASK_ID[7:4]: TS, TREG, BAT(charge status), PG interrupts masked -> /INT fires for button presses only.
     * Without a battery the charger flips CV<->done every 2 s and would storm /INT. Charger state is polled. */
    bq25180_write(BQ25180_MASK_ID, 0xF0);

    uint8_t s0 = 0, ichg = 0; bq25180_read(BQ25180_STAT0, &s0); bq25180_read(BQ25180_ICHG_CTRL, &ichg);
    ESP_LOGI(TAG, "id %02X, ICHG_CTRL %02X, VIN_PGOOD %d, %s", id, ichg, s0 & 1, bq25180_chg_name((s0 >> 5) & 3));
    return ESP_OK;
}

static volatile uint8_t s_wake_latch;

static esp_err_t read_stat1(uint8_t *v)
{
    esp_err_t e = bq25180_read(BQ25180_STAT1, v);
    if (e == ESP_OK) s_wake_latch |= *v & 0x03;        /* bit1 WAKE1_FLAG, bit0 WAKE2_FLAG — never lose a press */
    return e;
}

uint8_t bq25180_peek_wake_flags(void) { return s_wake_latch; }

uint8_t bq25180_take_wake_flags(void)
{
    uint8_t v = 0; read_stat1(&v);
    uint8_t w = s_wake_latch; s_wake_latch = 0;
    return w;
}

esp_err_t bq25180_get_status(bq25180_status_t *st)
{
    esp_err_t e = bq25180_read(BQ25180_STAT0, &st->stat0);
    e |= read_stat1(&st->stat1);
    e |= bq25180_read(BQ25180_FLAG0, &st->flag0);
    st->vin_pgood      = st->stat0 & 0x01;
    st->thermreg_active= st->stat0 & 0x02;
    st->vindpm_active  = st->stat0 & 0x04;
    st->vdppm_active   = st->stat0 & 0x08;
    st->ilim_active    = st->stat0 & 0x10;
    st->chg            = (st->stat0 >> 5) & 3;
    st->ts_open        = st->stat0 & 0x80;
    st->safety_fault   = st->stat1 & 0x04;
    return e;
}

esp_err_t bq25180_enter_ship_mode(void)
{
    /* SHIP_RST bits[6:5] EN_RST_SHIP = 10: ship mode, wake on button press or VIN. Takes effect
     * after VIN is removed (datasheet) — VERIFY on hardware in the power block. */
    uint8_t v = 0; bq25180_read(BQ25180_SHIP_RST, &v);
    esp_err_t e = bq25180_write(BQ25180_SHIP_RST, (v & 0x1F) | 0x40);   /* REG_RST=0, EN_RST_SHIP=10, keep PB/WAKE/EN_PUSH */
    uint8_t rb = 0; bq25180_read(BQ25180_SHIP_RST, &rb);
    ESP_LOGW(TAG, "ship mode requested: SHIP_RST %02X -> %02X (%s)", v, rb, e == ESP_OK ? "written" : esp_err_to_name(e));
    return e;
}

const char *bq25180_chg_name(bq_chg_stat_t s)
{
    static const char *n[] = { "not charging", "charging (CC)", "charging (CV)", "charge completed" };
    return n[s & 3];
}
