/* Board self-test: runs on the initialised drivers, records a PASS/FAIL table (exposed on /api/v1/selftest). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { const char *name; bool pass; char info[72]; } selftest_result_t;

/* Flags to pick which groups run (the long ones are optional at boot). */
#define ST_SOC      (1 << 0)
#define ST_I2C      (1 << 1)   /* scan + BQ25180 + LP5810 colour cycle (1.6 s) */
#define ST_IMU      (1 << 2)   /* WHO_AM_I, gravity both channels, DRDY rate (1.5 s) */
#define ST_NAND     (1 << 3)   /* ID, unlock, one block erase/program/verify */
#define ST_NAND_STRESS (1 << 4) /* 2 MB program/verify at 20 + 40 MHz (~5 s) */
#define ST_POWER    (1 << 5)   /* VBAT sanity; button press window */
#define ST_WIFI     (1 << 6)   /* scan (needs net_init) */
#define ST_QUICK    (ST_SOC | ST_I2C | ST_IMU | ST_NAND | ST_POWER | ST_WIFI)
#define ST_ALL      (ST_QUICK | ST_NAND_STRESS)

void selftest_run(uint32_t groups, uint32_t button_wait_s);
int  selftest_count(void);
const selftest_result_t *selftest_result(int i);
int  selftest_passed(void);
