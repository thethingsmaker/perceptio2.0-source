# Perceptio 2.0 — project knowledge (read first)

PadelSense SRL smart-racket sensor board ("POGO", PADELIO 5.0, product name Perceptio 2.0). Owner: Alberto.
This folder is the **bring-up firmware** (PlatformIO + ESP-IDF 5.x). The production firmware will be native ESP-IDF 6.x,
per `PADELIO40_firmware_rewrite_prompt.md` (the brief, kept in the Claude Project "Padel Shots Analisys"). Nothing here is production code.

Working style: metric units, English or Italian, no fluff, verify against datasheets/driver sources, say so when something is unverified.

## Hardware (rev 20, verified on real boards 2026-10-21 — 24/24 bring-up tests pass)

- MCU **ESP32-S3-MINI-1-N4R2**: 4 MB flash DIO 80 MHz, 2 MB quad PSRAM. IO26 reserved. Strapping IO0 (R56 10k up), IO3 (R54 10k down), IO45, IO46. USB-Serial-JTAG on IO19/IO20 (pogo pads; power-only cables will not enumerate).
- IMU **ST LSM6DSV320X** on SPI3: SCK IO18, MOSI IO21, MISO IO33, CS IO15 (R55 10k to 3V3), INT1 IO5, INT2 IO17. WHO_AM_I 0x73. 10 MHz mode 0. First write must be `IF_CFG (0x03) = 0x01` (disable I2C/I3C).
  Bring-up config: CTRL3 0x44 (BDU, IF_INC), CTRL6 0x0D (±4000 dps; bit3 must be 1), CTRL8 0x03 (±16 g), CTRL1/CTRL2 0x09 (HP mode, 960 Hz), CTRL4 0x02 + INT1_CTRL 0x01 (DRDY pulsed on INT1 → measured 927 pulses/s, oscillator tolerance).
  **High-g channel** (from ST driver `lsm6dsv320x_reg.h/.c`, cloned and read): `CTRL1_XL_HG` 0x4E = `regout_en(bit7) | ODR[5:3] | FS[2:0]`; ODR 3=480 4=960 5=1920 6=3840 7=7680 Hz; FS 0=32 1=64 2=128 3=256 4=320 g. Outputs 0x34–0x39. **±320 g sensitivity = 10.417 mg/LSB** (register spans ±341 g; never scale as fs/32768). Status bit `xlhgda` in 0x1E; DRDY routing `int1_drdy_xl_hg` = CTRL7 (0x16) bit7. Low-g must be HP or HA mode when HG is on. Bring-up uses 0xA4 (960 Hz, ±320 g).
  Hand taps already exceed 16 g → the high-g channel is mandatory for impacts, not optional. Die temperature reads ambient +10–15 K with WiFi on (42 °C seen) — matters because charging is gated on IMU temp (no NTC).
- NAND **Winbond W25N01KV** on SPI2 (IO_MUX): CLK IO12, MOSI IO11, MISO IO13, CS IO10, WP#/HOLD# pulled up (R102/R103). JEDEC `EF AE 21`. Ships with BP bits set → `WREN; WRSR(A0h)=0x00` first. SR2 (B0h) default 0x19 (ECC on, BUF=1). 2048 B pages, 64 pages/block, 1024 blocks. Pages programmed ascending. **0 factory bad blocks on this unit** (scan spare byte 0 of page 0 per block). Measured: program ≈1.27 MB/s, read ≈1.34 MB/s @20 MHz, ≈1.93 MB/s @40 MHz with Fast Read 0Bh — all 0 mismatches, ECC 0. 40 MHz qualified. Required for capture: 9.6 KB/s.
- I²C: **SCL IO37, SDA IO36**, 4.7 k pull-ups. BQ25180 @0x6A. LP5810 answers at 0x5C–0x5F **and** 0x6C–0x6F (broadcast range) — expected, not ghosts.
- Charger **BQ25180YBGR**: MASK_ID 0xC0. `ICHG_CTRL (0x04) = 0x4D` → 500 mA (default 10 mA; the 160 s watchdog resets registers unless disabled → re-arm or disable). STAT0: bit0 VIN_PGOOD, bits[6:5] CHG_STAT 0 none/1 CC/2 CV/3 done. With no battery it reports "done" and regulates BAT to ~4.2 V. Button → /INT on IO6 (pulses on press/release). VBAT: 1 M/1 M divider → IO4 = ADC1_CH3; raw ADC reads ~10 % low → use `esp_adc_cali`.
- LED **LP5810DDSDR** + FM-B2020RGBA-HG common anode: OUT0 R, OUT1 G, OUT2 B. Init: 0x000=1, 0x001=0, 0x002=0, 0x00D=0x0B, 0x010=0x55, check 0x300==0, 0x020=0x07, DC 0x030..0x032 (R 0x40, G/B ≤0x1E for Vf headroom), PWM 0x040..0x042.
- Battery EEMB LP803030 700 mAh — fitted on the test board …fcc. No reverse-polarity protection on the BAT pin.

## Firmware rules (apply to everything written for this board)

- **Core split is a hard requirement**: core 0 = WiFi, lwIP, esp_timer, HTTP/UI; core 1 = capture (IMU, impact, NAND writer), GPIO ISR service installed *from core 1*. IMU SPI via `spi_device_polling_transmit` (no SPI ISR). Set in `sdkconfig.defaults` (`ESP_MAIN_TASK_AFFINITY_CPU0`, `ESP_WIFI_TASK_PINNED_TO_CORE_0`, `LWIP_TCPIP_TASK_AFFINITY_CPU0`, `ESP_TIMER_TASK_AFFINITY_CPU0`) and `httpd` `core_id = 0`.
- **No internal-flash writes during a capture session** (NVS, OTA, coredump) — they stall both cores. The same holds for anything that
  maps or reads the flash at run time (`esp_ota_get_state_partition`, `esp_partition_read/mmap`, `nvs_get_*`): read such values once at
  boot and keep them in RAM — a 10 Hz otadata read in the main loop caused interrupt-watchdog resets mid-recording (0.3.20 fix). Settings changed mid-session go to a CONFIG record on NAND; NVS commit at session stop.
- **Nothing big in internal RAM by accident.** A plain `static` array lands in internal `.bss`. Anything that is not a DMA
  buffer, not used by an ISR or during flash operations, not a TLS/AES buffer and not the stack of a task that writes the
  flash is allocated in PSRAM at init (`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`). Check after adding buffers:
  `python -m esp_idf_size --archives <build>/*.map` → `libsrc.a` `.bss` (43 KB since 0.3.25; it was 118 KB, 80 KB of it the
  Live-tab sample ring in `webui.c`).
- **Main task stack 6144** (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`): `app_main` runs self-test, status, LED/power logic and OTA
  validation. At IDF's 3584 it had 192 B left; an overrun corrupted the partition list and crashed `ota_mark_valid`
  (LoadProhibited in `esp_partition_next`, 2026-10-29). The `DIAG:` line (once a minute, idle only) lists the 8 tasks with
  the least stack left; `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` panics on an overrun and names the task.
- **No kernel-lock work while recording.** The IMU is read from its output registers on each data-ready edge (no FIFO);
  anything that holds the FreeRTOS kernel lock or masks interrupts on core 1 for > 1 period (1.09 ms) merges two edges and
  loses a sample — `uxTaskGetSystemState` (the DIAG report, ~2 ms) did exactly that. The store flags intervals > 1.5 periods
  (`gaps`, `gap_max_us` in END2 and `rec_stopped`, lost-samples flag on the block); `dropped` only counts a full ring.
- **OTA runs in update mode**: check and install destroy the MQTT client (frees ~29 KB, measured) and announce `state off
  (fw_update)`; an installed image reboots, a failed one resumes the link. The phase is set busy *before* that, or the UI
  stops polling.
- **Crash evidence is kept**: the core dump stays in flash after its one-line report (`cd_seen` = its CRC in NVS) until
  `DELETE /api/v1/coredump`; download it with `GET /api/v1/coredump` and decode with
  `esp-coredump info_corefile -t raw -c <file> .pio/build/perceptio2/firmware.elf` (same build!). Log time ≠ sample time:
  the log clock ran ~0.54 s ahead of esp_timer; `recording … started` prints both.
- **Switching off = ship mode, checked at the next boot.** Ship mode cannot engage with the cable in (→ CHARGE-IDLE)
  and a request can be ignored, so `power_mgr` leaves an RTC breadcrumb and falls back to deep sleep after 6 s; the boot
  report says which happened (README "Power"). Never call `bq25180_enter_ship_mode()` directly — use `power_mgr_off()`.
- WiFi modem sleep adds 100–300 ms per HTTP request → `esp_wifi_set_ps(WIFI_PS_NONE)` while the UI is in use (parameter `wifi_ps`).
- The web page is just a client of `/api/v1` (see `API.md`); the mobile app uses the same API. Parameters are self-describing (`GET /api/v1/schema` from `PARAM_TABLE` in `src/config.c`) — add a parameter = one table line, never hard-code keys in clients.
- Passwords never leave the device over the API.
- **Versioning**: every change committed to the PC bumps the patch number in `include/config.h` (`FW_VERSION_PATCH` + `FW_VERSION_STR`, kept equal) — 0.3.2, 0.3.3, … until Alberto calls 0.4.0. GitHub release tags are `v<FW_VERSION_STR>`; the OTA check compares them numerically.
- I2C driver and GPIO ISR service are installed from a core-1 task (`board.c`): with the I2C ISR on core 0, TLS load made transactions time out and the legacy driver's FSM reset storm tripped the interrupt watchdog. The BQ25180 charge profile (VBAT_CTRL 0x46, CHARGECTRL0/1) is written as absolute values at every boot — corrupted registers (VBATREG 3.51 V) once made it report "charging" with zero current.

## PlatformIO quirks learned here

- Config file is `sdkconfig.perceptio2`; it is regenerated only when `sdkconfig.defaults` is newer — if a new `CONFIG_*` does not take effect, delete it and rebuild. Partition table changes need `pio run -t erase` once (otadata/nvs offsets moved).
- Neither CMake `EMBED_FILES` nor `board_build.embed_files` works reliably with `framework = espidf` → the web page is embedded as a C array (`tools/gzip_web.py` → `src/web_index.c`, pre-build hook).
- `-Werror=misleading-indentation` is on: no one-line `if … stmt, stmt;`.
- Editor "stdio.h not found" squiggles before the first build are IntelliSense, not the compiler.
- `espressif/mdns` is pulled by `src/idf_component.yml` (needs internet on first build). Guarded by `__has_include("mdns.h")`.
- Legacy `driver/i2c.h` is used (deprecation warnings are fine on IDF 5.x).

## Layout (since block 1, fw 0.0.3)

One driver per file under `src/` with its header in `include/`: `board` (pins, buses, ISR service on core 1), `bq25180`, `lp5810`,
`imu` (wrapper over the vendored ST driver in `components/lsm6dsv320x`), `w25n01kv`, `power` (VBAT calibrated ADC + /INT hook),
`net` (WiFi list join, keepalive, mDNS), `sampler` (core-1 DRDY task with sinks), `selftest` (bring-up tests on the drivers,
`/api/v1/selftest`), `config`, `webui`. `partitions.csv`: two OTA slots, no factory app. See README for the table.
Since refactor step 3 (0.3.28–0.3.32) `main.c` is init + a 100 ms tick, and the policies live in `button` (gestures),
`netwatch` (fallback AP), `led_policy` (LED priority) and `power_mgr` (ON/OFF/CHARGE-IDLE, charge gate, low battery,
boot report). Main passes them hooks (backend reachable, prepare-for-off, …), so they do not depend on cloud/store.

Sensitivities come from the ST driver, never fs/32768: accel 0.488 mg/LSB @±16 g; **gyro 0.035 × FS mdps/LSB (140 @±4000)**;
high-g 10.417 mg/LSB @±320 g. `/info` publishes `xl_mg_lsb, gy_mdps_lsb, hg_mg_lsb`; clients must use them.
All channels share one ODR (480/960/1920) because the HG ladder has no 800 Hz; default 960 Hz, 20 B/sample stored as-is.
Plan (agreed 2026-10-22): no impact-event detection on device — the high-g channel is just recorded with the rest; analysis offline.

Later modules: `clock` (SNTP, TZ, sync hook), `cloud` (MQTT5/WSS per `docs/protocol-v2.md`), `store` (fw 0.3.0: PCR2 files on the NAND per
`docs/data-v2.md`, codec-1 blocks of 400 samples sliced into 2032 B pages, container header in `include/store.h`, validate with `tools/pcr2_check.py`),
`upload` (resumable HTTPS per data-v2 §2, driven by the backend's `upload` command). Store rules: blocks 8..1019 data, 0..7/1020..1023
reserved (self-test scratch only); every page carries a 16 B header so the inventory is a boot-time scan, no directory;
`config_set_locked()` while recording (PATCH /config → 409); NAND reads (download/delete) refused while recording; the
session is always closed with a trailer before power-off/timeout/storage-full. Capacity ≈ 110 min at 960 Hz.

## Memory and upload throughput (re-measured 2026-10-29, fw 0.3.27 — do not re-tune without new data)

Internal RAM free (fw 0.3.27, board fcc): after drivers 250 KB, after Wi-Fi join 158 KB, **idle ≈ 123 KB** with Wi-Fi +
MQTT/TLS + HTTP server; **≈ 100 KB minimum during an upload** (Live tab open); ≈ 145 KB in OTA update mode, ≈ 114 KB
during the download. Until 0.3.24 it was 29–35 KB idle and 9–13 KB during uploads: 94 KB of our own static arrays sat in
internal RAM (see Firmware rules). The boot log prints `heap after drivers / wifi init / wifi join`, the 10 s status line
prints the heap, `upload` acks carry `stats.heap_min_kb`, OTA progress lines carry `heap … (min …)`. Lessons, each paid for:
- A TLS session needs ~30 KB when mbedtls allocates internally; the upload opened a second one → `mbedtls_ssl_setup -0x7F00`.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` keeps the 64 KB TCP send buffer (receive window 32 KB since 0.3.27) in PSRAM but Wi-Fi buffers must be DMA-capable, so it adds
  internal "cache TX buffers" (`ESP_WIFI_CACHE_TX_BUFFER_NUM`, min 16 ≈ 26 KB). Static RX buffers 10, RX BA window ≤ 2×that.
- Hardware AES on a PSRAM buffer allocates an internal DMA bounce buffer per TLS record: with < ~20 KB free this fails
  (`esp-aes: Failed to allocate memory` → dropped connection). Hence TLS records 4 KB and TLS output buffers internal
  (`MBEDTLS_DEFAULT_MEM_ALLOC` + `SPIRAM_MALLOC_ALWAYSINTERNAL=6144`), and `MIN_HEAP` guard 24 KB before opening the session.
- Non-critical task stacks (upload, reader, NAND writer, WS sender, LED) live in PSRAM (`include/task_psram.h`); tasks that
  write NVS (main, cloud) stay internal. `LWIP_MAX_SOCKETS=16` (MQTT + 7 HTTP + mDNS exceeded 10).
- Throughput: 106 KB/s (5.7 KB TCP window) → 380 (64 KB window: no gain, CPU-bound) → 500 (CPU 160→240 MHz) → ~530 KB/s
  (TLS buffers internal, BA window 32). ~0.17 s of each 512 KB chunk is the server's processing; 1 MB chunks (backend's
  `chunk_bytes`) are the one lever left. A 40 MB codec-1 match uploads in ~75-90 s. Accepted as the ceiling.
- OTA downloads stalled 31–62 s (TCP back-off after dropped frames) in 3 of 4 runs with ~20–25 KB free, none with ~115 KB
  free: on this board, low internal RAM shows up as network stalls before it shows up as allocation errors.
- `sdkconfig.perceptio2` must be deleted after every `sdkconfig.defaults` change; the `UPLOAD: build:` boot line shows the
  effective values — check it before believing a tuning result.
- esp-mqtt resends every unacked QoS1 publish on reconnect; with the broker's 20-in-flight limit that becomes an endless
  reconnect loop → the client is destroyed/recreated on disconnect and events are published ≤ 8 in flight (`EV_WINDOW`).

## Open items

- Battery: on-battery operation, ship mode and low-battery shutdown verified (0.3.33/0.3.34, board …fcc). Still open:
  a full charge from a low cell (CC→CV→DONE); no battery → disable charging and show "no battery".
- Fallback AP UX: the LED blinks blue (net bad) ~70 s before the AP exists; the AP channel follows the station's join
  attempts (one radio); a phone that stays connected keeps the AP up; `wifi_ps` is not restored after the AP stops.
- Power-off: `cloud_shutdown` runs from the main task while the cloud task may still be active (possible minor race).
- OTA task stack (7168) could shrink to ~5.5 KB (measured peak). Step 4: feature on/off switches. BLE setup mode
  (measure RAM first; ~123 KB free idle now).
- ADC calibration; BQ watchdog policy; LED brightness vs Vf.
- Next blocks: events mirrored to NAND, cloud `configure` command, secure boot per `PADELIO40_securing_platform_procedure.md` (OTA and the AP fallback are done).
- Diagnostics added for the 2026-10-29 crash hunt stay until a few days of clean DIAG lines: end-of-stack watchpoint and trace facility (`sdkconfig.defaults`), `src/diag.c`.
- 800 Hz adequacy ablation once labelled data exists.
