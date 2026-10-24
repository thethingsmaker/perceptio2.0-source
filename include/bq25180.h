/* TI BQ25180 linear charger — I2C 0x6A. Register map from the datasheet (SLUSD91). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BQ25180_ADDR        0x6A
#define BQ25180_STAT0       0x00
#define BQ25180_STAT1       0x01
#define BQ25180_FLAG0       0x02
#define BQ25180_VBAT_CTRL   0x03
#define BQ25180_ICHG_CTRL   0x04
#define BQ25180_CHARGECTRL0 0x05
#define BQ25180_CHARGECTRL1 0x06
#define BQ25180_IC_CTRL     0x07
#define BQ25180_TMR_ILIM    0x08
#define BQ25180_SHIP_RST    0x09
#define BQ25180_SYS_REG     0x0A
#define BQ25180_TS_CONTROL  0x0B
#define BQ25180_MASK_ID     0x0C

typedef enum { BQ_CHG_NONE = 0, BQ_CHG_CC = 1, BQ_CHG_CV = 2, BQ_CHG_DONE = 3 } bq_chg_stat_t;

typedef struct {
    uint8_t      stat0, stat1, flag0;   /* raw; FLAG0 clears on read */
    bool         vin_pgood;             /* USB present and good */
    bq_chg_stat_t chg;                  /* charge state */
    bool         ilim_active, vdppm_active, vindpm_active, thermreg_active;
    bool         ts_open;
    bool         safety_fault;          /* STAT1 bit2: fast-charge safety timer expired -> charging stopped until CE toggles */
} bq25180_status_t;

esp_err_t bq25180_read(uint8_t reg, uint8_t *v);
esp_err_t bq25180_write(uint8_t reg, uint8_t v);

/* Probe + apply the board defaults: ICHG 500 mA, watchdog disabled (so settings persist). */
esp_err_t bq25180_init(void);
esp_err_t bq25180_get_status(bq25180_status_t *st);
/* STAT1 is read-to-clear: every read in the driver latches WAKE1/WAKE2 (bits 1/0). This returns and clears the latch. */
uint8_t   bq25180_take_wake_flags(void);
uint8_t   bq25180_peek_wake_flags(void);   /* latch content without clearing (diagnostics) */
/* Set charge current in mA (ICHG_CTRL encoding: ≤35 mA → code = mA-5; ≥40 mA → code = 40+(mA-40)/10). */
esp_err_t bq25180_set_ichg_ma(uint16_t ma);
/* Enable/disable charging (CHARGECTRL... CHG_DIS bit in ICHG_CTRL bit7). */
esp_err_t bq25180_charge_enable(bool en);
/* Input current limit: 50/100/200/300/400/500/700/1100 mA (TMR_ILIM[2:0]); the charger shares it with the board. */
esp_err_t bq25180_set_ilim_ma(uint16_t ma);
/* Safety-timer fault: toggle CE to restart the charge cycle (datasheet: "cleared only after CE is toggled"). */
esp_err_t bq25180_restart_charge(void);
/* Ship mode: ~nothing draws from the battery until the button or VIN wakes it. */
esp_err_t bq25180_enter_ship_mode(void);
const char *bq25180_chg_name(bq_chg_stat_t s);
