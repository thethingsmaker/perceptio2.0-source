# PADELIO 4.0 (POGO) — Firmware Rewrite Brief

You are rewriting the firmware for a smart padel racket sensor board (PadelSense / PADELIO 4.0_POGO). The board captures a continuous 800 Hz 6-axis IMU stream plus short high-g impact packets from a dedicated ±200 g accelerometer, stores everything on local NAND flash, and uploads after the match. Everything below was decided and verified against datasheets during the hardware design session. Treat it as the spec. Items marked **VERIFY** were not confirmed and must be checked against the datasheet or on the bench before being relied on.

Communicate in English or Italian as the user does. The user is a senior embedded engineer: be concise, technically rigorous, no padding. Deliver ready-to-compile code, not instructions to go find parameters.

---

## 1. Target hardware

**MCU module:** ESP32-S3-MINI-1-N4R2
- 4 MB Quad-SPI flash, 2 MB Quad-SPI PSRAM (in-package), dual-core Xtensa LX7 @ 240 MHz
- IO26 is wired to the embedded PSRAM — never use it
- IO27–IO32 not exposed on the module
- IO33–IO37 are free (quad PSRAM, not octal)
- Strapping pins IO0, IO3, IO45, IO46 — do not repurpose
- Only 2 MB PSRAM total: WiFi/BT stack, TLS, and the capture buffers all share it. Budget explicitly; measure with `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` under WiFi load before sizing buffers.

**Toolchain:** Arduino IDE with arduino-esp32 core. ESP-IDF headers are available directly (`driver/spi_master.h`, `esp_heap_caps.h`, FreeRTOS). **VERIFY** which arduino-esp32 version (2.x = IDF 4.4, 3.x = IDF 5.x) — this changes the PSRAM DMA buffer alignment rules (see §5).

### GPIO map (schematic PADELIO4.0_POGO, rev 2026-09-14, plus the SPI additions below)

| GPIO | Net | Notes |
|---|---|---|
| IO3 | — | strapping, R54 10k pull-down, leave alone |
| IO0 | — | strapping, R56 10k pull-up + test pad |
| IO4 | VBAT_SENS | ADC1, 1 MΩ/1 MΩ divider + 100 nF |
| IO5 | INT_IMU | LSM6DSV32X INT1 (FIFO watermark) |
| IO6 | INT_BQ | BQ25180 /INT |
| IO7, IO8 | — | free (SK6805 circuit removed; status LED is now the LP5810 on I²C) |
| IO9 | FSPIHD | NAND HOLD#/IO3 |
| IO10 | FSPICS0 | NAND CS# |
| IO11 | FSPID (MOSI) | NAND DI/IO0 |
| IO12 | FSPICLK | NAND CLK |
| IO13 | FSPIQ (MISO) | NAND DO/IO1 |
| IO14 | FSPIWP | NAND WP#/IO2 |
| IO15 | CS_IMU | IMU CS on SPI3, 10k pull-up to 3V3 |
| IO16 | CS_HIG | ADXL372 CS on SPI3 (fallback build), 10k pull-up to 3V3 |
| IO17 | INT_HIG | ADXL372 INT1 (fallback build) / IMU INT2 (high-g shock, single-chip build) |
| IO18 | SPI3 CLK | sensor bus clock (GPIO matrix) |
| IO21 | SPI3 MOSI | sensor bus |
| IO33 | SPI3 MISO | sensor bus |
| IO19 / IO20 | USB D− / D+ | native USB, 22 Ω series, USBLC6-2SC6 ESD |
| IO37 / IO36 | **SCL / SDA** (note: SCL = IO37, SDA = IO36 per final netlist) | I2C 400 kHz, 4.7k pull-ups — BQ25180 (0x6A) and LP5810 LED driver |
| IO0, EN, RXD0, TXD0 | test pads | recovery only |

Free for future use: IO34, IO35, IO38–IO48 (minus strapping).

### Peripherals

**IMU — ST LSM6DSV32X** (replaces LSM6DSO32; **VERIFY** LGA-14 pinout against the DSV32X datasheet Table 2 — the DSV adds I3C and aux-OIS SPI pins that were NC on the DSO32; tie them off, do not float)
- Accel ±4/8/16/32 g, gyro ±125…±4000 dps
- ODR: use **HAODR set 2** (`HAODR_CFG (62h)`, `HAODR_SEL[1:0] = 10`) which gives 400/800/1600/3200 Hz. The standard table (480/960/1920/3840) has no 800 Hz. HAODR must be written while both sensors are in power-down and applies to both; +20 µA; incompatible with activity/inactivity (unused). It also tightens part-to-part ODR accuracy, which helps video sync.
- 4-wire SPI mode 0 or 3, CS active low; CS high at power-up = I2C mode, so CS is pulled up and the driver must assert it deliberately
- VDD and VDDIO both 3V3
- Hardware FIFO with watermark interrupt on INT1 → IO5. ST advertises "up to 4.5 KB"; **VERIFY** the *uncompressed* physical depth — the jitter margin in §3 depends on it
- WHO_AM_I: **VERIFY** value in datasheet (do not assume 0x70)
- Driver: **STMicroelectronics/lsm6dsv32x-pid** (GitHub, BSD-3). Drop `lsm6dsv32x_reg.c/.h` into the sketch. Implement `platform_write()` / `platform_read()` on top of ESP-IDF `spi_master` (§4). Set bit 7 of the register address for SPI reads.

**High-g accelerometer — Analog Devices ADXL372** (populated, not DNP)
- ±200 g, 3 axes, 12-bit (100 mg/LSB), ODR 400–6400 Hz, bandwidth to 3200 Hz, 22 µA at 3.2 kHz
- **512-sample FIFO** — samples are per axis, so 3-axis sets: at most ~169 per read (datasheet: leave ≥1 set in FIFO). At 3200 Hz that is **~53 ms** of 3-axis data; at 6400 Hz ~26 ms.
- Modes: bypass / stream / **triggered** / oldest-saved; configure only in standby. Impact detection with `THRESH_ACT`/`THRESH_INACT`, instant-on mode, peak-only FIFO format, adjustable high-pass and antialiasing filters.
- SPI 4-wire on SPI2, CS IO16, INT1 → IO17. LGA-16 3×3.25 mm.
- Driver: ADI `no-OS/drivers/accel/adxl372` (register map + FIFO config) or the ADI Arduino example; port the bus layer to `spi_master` exactly as for the IMU.
- Role: 3.2 kHz, ±200 g close-up of each ball contact and the first ~50 ms of frame ringdown. It does **not** produce a continuous stream.

**Storage — Winbond W25N01KVZEIR** (1 Gbit SLC SPI NAND, WSON-8 8×6, datasheet Rev H 2023-10-16)
- 128 MB = 1024 blocks × 64 pages × 2048 B (+64 B spare, of which 32 B parity). 128 KB erase block.
- SPI up to 104 MHz, mode 0/3. 25 mA active, 10 µA standby. 60 000 P/E cycles. On-chip 4-bit ECC.
- Pull-ups 10k on CS#, WP#, HOLD#. 100 nF on VCC pin 8. WP-E stays 0 (software protection mode) so WP#/HOLD# are usable as IO2/IO3 for Quad later.
- **Not supported by ESP-IDF `esp_flash`, partitions, FAT/SPIFFS/LittleFS, or esptool** (NOR only). The driver is custom, raw `spi_master` transactions. This is by design — see §6.

**Charger / power — TI BQ25180YBGR**, I2C 0x6A, TS/MR pin implements the single button: 3 s press = on, 5 s = off (hardware power-cycle path for crash recovery; firmware must not rely on it for normal shutdown). Charge status read from register STAT0 over I2C, no CHG pin. /INT → IO6.

**BQ25180 register configuration (write at boot; datasheet SLUSE99C):**
| Register | Value | Why |
|---|---|---|
| `ICHG_CTRL` (0x04) | `0x4D` = 500 mA | reset default is **10 mA**; 500 mA is the thermal ceiling for the DSBGA on a 2-layer board |
| `VBAT_CTRL` (0x03) | default `0x46` = 4.20 V | correct for LP803030 |
| `TMR_ILIM` (0x08) | `MR_LPRESS=00` (5 s), `ILIM=101` (500 mA) | 5 s long press; USB-port-safe input limit |
| `SHIP_RST` (0x09) | `PB_LPRESS_ACTION=01` (hardware reset), `WAKE2_TMR=1` (3 s), `EN_PUSH=1` | 5 s hold = hardware power cycle even if the S3 is hung; 3 s = WAKE2 interrupt → firmware closes the session and writes `EN_RST_SHIP=10` (ship mode) |
| `IC_CTRL` (0x07) | `WATCHDOG_SEL=11` (disabled) or poll STAT0 every ≤60 s | otherwise all charge registers **reset to defaults (10 mA) after 160 s** without I²C traffic |
| `CHARGECTRL1` (0x06) | `CHG_STATUS_INT_MASK=0` | get charge-state changes on /INT for the LED |
| `SYS_REG` (0x0A) | default 4.5 V | LDO input when adapter present |
| `ICHG_CTRL.CHG_DIS` | set/cleared by the temperature gate | no NTC on the pack: gate charging on the IMU die temperature (~5–40 °C, with hysteresis) |
/INT is a 128 µs active-low pulse → edge-triggered GPIO interrupt on IO6. Wake from ship mode: button ≥2 s or adapter insertion.

**Battery:** EEMB LP803030, 3.7 V 700 mAh, built-in PCM, 500 mA continuous / 1 A peak.

**LED:** TI LP5810 4-channel I²C LED driver (address per DDSDR variant — **VERIFY** in datasheet) driving an FM-B2020RGBA-HG common-anode RGB LED; **OUT0 = R, OUT1 = G, OUT2 = B**, OUT3 unused (disable in config), SYNC to GND. Autonomous animation engine: write a pattern once per state (idle/recording/charging/uploading/error); use the driver's shutdown mode in sleep. Run G/B at ≤3 mA for headroom at 3.3 V.

**Connector:** 4-pin magnetic pogo (Xinyangze YZ103915020T-04025-02): power + USB D±. VIN (pin 4) passes through Q21 AO3401A reverse-polarity P-FET (gate R21 100k to GND) to VBUS; SMF5.0A TVS on VIN; USBLC6-2SC6 on D±. The shipped cable carries power only; D± exist for recovery/flash via USB without opening the case.

**Racket reference frame (used by everything — silkscreen, log format, backend):** X = along the handle toward the head, Y = lateral across the face, Z = normal to the face, right-handed.

**Axis alignment of the two accelerometers:** the LSM6DSV32X and ADXL372 packages are rotated on the PCB so that their datasheet axes fall on the racket axes (the two vendors use different pin-1 conventions — check each datasheet's axis figure). Residual misalignment is corrected by a per-sensor 3×3 rotation matrix stored in NVS and written into every `SESSION_HEADER`; never assume identity. The two chips are placed as close together as the footprints allow, along X, and the centre-to-centre offset `r_hig_vs_imu` (mm, racket frame) is stored in the header so the backend can refer the ADXL372 trace to the IMU location (`a = a_imu + α×r + ω×(ω×r)`).

**Sensor position on the racket:** throat, ~4 cm above the handle end, close to the centre of mass. Expect 20–40 g of swing centripetal load on hard smashes *before* impact; the IMU accelerometer may saturate briefly on remates — that is why the ADXL372 exists.

---

## 2. Capture requirements

### IMU stream (continuous)
- **Accel 800 Hz + gyro 800 Hz**, both in HAODR set 2. int16 per axis → **12 B per sample-set, 9.6 KB/s**. One record type, one index, no per-sensor streams.
- **120 min worst case → 69 MB per session.** 128 MB NAND holds ~3.7 h (1.85× headroom).
- Accel full scale ±32 g, gyro ±4000 dps. Accel LPF: widest bandwidth allowed at 800 Hz; gyro LPF default.
- No per-sample timestamps: fixed ODR + session header (start time UTC, ODR, FS, channel layout, firmware version, device ID) + sample index. Marker record on any ODR/FS change. Optionally enable the FIFO timestamp tag at ~1 Hz as a drift cross-check.
- FIFO tag bytes are stripped; store packed int16 rows only.
- Per-session saturation counter (samples at ±32767 on any IMU accel axis) in the trailer.

### Impact packets (event-driven)
- ADXL372 at **3200 Hz, bandwidth 1600 Hz**, FIFO in **triggered mode**, XYZ format, with the on-chip activity/impact threshold as trigger. Threshold ~40–50 g (above swing loads, below any real contact peak) — **VERIFY** empirically in the first sessions; also record the peak-only value.
- FIFO allocation: ~1/3 pre-trigger, ~2/3 post-trigger (e.g. 50 + 118 sets ≈ 15 ms before + 37 ms after contact).
- On INT1 (IO17): capture the IMU sample index (or `esp_timer_get_time()`), read the FIFO in one DMA burst, re-arm (standby → triggered), write an `IMPACT_EVENT` record.
- Size: ~170 sets × 6 B ≈ 1 KB + header per impact; ~600 impacts/match ≈ 0.6 MB. Negligible.
- Blind window between trigger and re-arm must stay < 5 ms; log any impact interrupt that arrives while un-armed.

---

## 3. Task / core architecture

The second core is already committed to WiFi connectivity and UI responsiveness. Do **not** move that. Capture and storage tasks are co-located on the WiFi/UI core and isolated by **priority**, not by core:

1. WiFi/BT stack tasks — untouched, highest of this group
2. **IMU sampling task** — above UI, below WiFi. Wakes on INT1 (IO5) FIFO watermark (64–128 FIFO words ≈ 40–80 ms cadence at 800/800), issues one DMA burst read of the FIFO, appends packed rows to the active PSRAM buffer, sleeps. No allocation, no flash access.
3. **Impact task** — same priority as the IMU task. Wakes on ADXL372 INT1 (IO17), reads FIFO by DMA, re-arms, hands an `IMPACT_EVENT` record to the writer via the same buffer/queue mechanism.
4. **Flash writer task** — above UI, below sampling. Wakes when a buffer is handed over, streams it to NAND page by page, marks the buffer free.
5. UI task — lowest of this group

**ISR rule:** both GPIO ISRs only clear the flag, capture a timestamp/index, and notify their task (`vTaskNotifyGiveFromISR`). Never touch SPI in an ISR.

**Producer/consumer:** single-producer single-consumer with an atomic active/standby pointer swap and a 1–2 slot `xQueueCreateStatic` queue. No mutex on the sampling path.

**Jitter budget:** the IMU's hardware FIFO. At 800/800 it fills at 1600 words/s × 7 B ≈ 11 KB/s; a 3 KB physical FIFO gives ~270 ms, a 1.5 KB one ~135 ms — either is an order of magnitude beyond WiFi/UI scheduling jitter. **VERIFY** the DSV32X physical depth. The ADXL372 FIFO in triggered mode freezes after the trigger, so CPU latency cannot lose impact data; it only extends the blind window.

---

## 4. SPI bus and DMA

- **Two hosts, split by risk, not by sensor type:**
  - **SPI2 (FSPI, IO_MUX pins IO9–IO14): NAND only.** The only device needing IO_MUX speed, and the only one whose traffic must never reach a sensor.
  - **SPI3 (GPIO matrix: CLK IO18, MOSI IO21, MISO IO33): the sensors** — IMU on CS IO15; in the two-chip fallback also the ADXL372 on CS IO16. 10 MHz each; the GPIO-matrix limit (26 MHz full-duplex) is irrelevant.
  - The S3 has only these two general-purpose hosts (SPI0/1 are the internal flash/PSRAM). Each host gets its own GDMA channel and its own ISR; set `isr_cpu_id = ESP_INTR_CPU_AFFINITY_1` on **both**.
- `spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO)` — DMA is enabled at bus init and used automatically for transfers above the ~64-byte hardware FIFO. The IMU bursts (~900 B), ADXL372 FIFO reads (~1 KB) and NAND page loads (2 KB) all qualify.
- Use `spi_device_queue_trans()` / `spi_device_get_trans_result()` (async), not `spi_device_transmit()` (blocking).
- **Do not use the Arduino `SPI` class** — it is a wrapper with no DMA path. Use `driver/spi_master.h` directly (compiles in Arduino IDE). `ESP32DMASPI` (hideakitai) is acceptable for a first version.
- GDMA on the S3 serves SPI2/SPI3 but **not I2C**. All three sensors/memories are on SPI for this reason. The BQ25180 stays on I2C.
- NAND and sensor transfers now overlap freely (separate hosts, separate DMA channels). Within SPI3, IMU reads take queue priority over ADXL372 reads.

### Clocks (verified against the S3 IDF limits and both datasheets)
| Device | Device max | S3-side limit | Setting |
|---|---|---|---|
| IMU (LSM6DSV320X / DSV32X) | **10 MHz** | 26.7 MHz (tv(SO) 25 ns) | `clock_speed_hz = 10 MHz`, `input_delay_ns = 25`, `mode = 0` |
| W25N01KV | 104 MHz | 80 MHz (tCLQV 7 ns) | `clock_speed_hz = 40 MHz` initially, `input_delay_ns = 7`, `mode = 0`; 80 MHz only after layout validation |
| ADXL372 (fallback build) | 10 MHz | — | `clock_speed_hz = 10 MHz`, `mode = 0` |

IO_MUX routing on IO9–IO14 is mandatory for the NAND host: through the GPIO matrix the S3 caps at 40 MHz (26 MHz full-duplex) — fine for the 10 MHz sensor host on SPI3, not for the NAND. Bus load worst case: SPI3 ~3 %, SPI2 ~1 % duty.

### Shared-bus hazard: the IMU's I²C engine (hard rule, still applies)
With CS high, ST IMUs keep their I²C/I3C decoder active on SCL/SDA. With the NAND moved to its own host the IMU no longer sees NAND traffic; in the two-chip fallback it still sees the ADXL372's reads on SPI3 (mode-0 SPI cannot form a clean I²C START, but trace skew can), so the rule stays:
- `IF_CFG (03h).I2C_I3C_disable = 1` must be the **first SPI transaction after power-up**, before any NAND access. `nand_init()` asserts that `imu_bus_locked == true`. The bit survives `SW_RESET` but not a power cycle.
- Keep the external 10k pull-up on IMU CS; the internal pull-up is disconnected once the bit is set.
- Bring-up test: after ≥10 min of NAND traffic with WiFi active, read back `CTRL1`, `CTRL2`, `CTRL6`, `FIFO_CTRL3`, `HAODR_CFG` and compare to the written values; any mismatch is a failed board.
- Both sensors tri-state MISO when deselected. IO9 (HOLD#) and IO14 (WP#) on the NAND host keep their 10k pull-ups even though the host is configured with `quadwp_io_num`/`quadhd_io_num`.

---

## 5. Buffers and memory discipline

The previous board generation had heap-fragmentation crashes severe enough to require a hardware power-cycle button. **Zero dynamic allocation in the steady-state capture path.**

- Two PSRAM ping-pong buffers, **128–256 KB each** (not 512 KB — 2 MB PSRAM is shared with WiFi). At 9.6 KB/s: 128 KB ≈ 13.6 s, 256 KB ≈ 27 s per buffer. Worst-case data loss on power failure = one buffer.
- Allocate once at boot: `heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)`. Length multiple of 4; alignment to cache line (32/64 B) for external RAM. **VERIFY** the exact requirement for the IDF version in use — a non-compliant buffer silently falls back to a bounce copy in internal RAM.
- Sizes must be an integer multiple of 2048 B (NAND page) so every flush is whole pages. Impact records are appended into the same buffer stream as typed records, so the log is a single append-only sequence.
- All queues, task stacks, and DMA descriptors static.
- No filesystem on the NAND (FAT/LittleFS metadata churn = allocation + non-deterministic latency).

---

## 6. NAND driver and on-flash layout

### Command set (W25N01KV, §8 of datasheet)
| Op | Opcode | Notes |
|---|---|---|
| JEDEC ID | 9Fh + 1 dummy | expect `EF AE 21` |
| Read SR | 0Fh/05h + addr | A0h SR-1 (protect), B0h SR-2 (config), C0h SR-3 (status) |
| Write SR | 1Fh/01h + addr + val | |
| Write Enable | 06h | required before every load/program/erase |
| Block Erase | D8h + 24-bit PA | 128 KB; poll BUSY, check E-FAIL |
| Load Program Data | 02h + CA[15:0] + data | resets unused buffer bytes to FF |
| Random Load | 84h + CA + data | partial update of buffer |
| Program Execute | 10h + 24-bit PA | poll BUSY, check P-FAIL |
| Page Data Read | 13h + 24-bit PA | into 2144 B buffer; tRD ≤ 60 µs with ECC |
| Read Data | 03h/0Bh + CA + dummy | from buffer |
| Reset | FFh | check BUSY=0 first |

Address: PA[15:6] = block (0–1023), PA[5:0] = page in block, CA[11:0] = byte in page.

### Mandatory bring-up (datasheet Fig. 11 defaults)
1. Read JEDEC ID — confirm bus and part
2. **BP[3:0],TB power up as `1111,1` = entire array write-protected.** Write Enable → Write SR-1 (A0h) with BP=0000, TB=0, WP-E=0, SRP=00. Without this every program/erase silently fails with P-FAIL/E-FAIL.
3. Leave ECC-E=1 (default). Leave BUF=1 (fixed on this part).
4. Build the bad-block table: for each block, Page Data Read page 0, read byte 0 of main area and byte 0 of spare (CA=0x800); non-FF = factory bad. Store the table in NVS.

### Rules
- Pages within a block **must be programmed in ascending order**. Random page programming is prohibited. The append-only log satisfies this naturally.
- Max 4 partial programs per page (NoP). Write whole 2048 B pages only.
- After every program: check P-FAIL. After every erase: check E-FAIL. After every read: check ECC-1/ECC-0 in SR-3. On failure, mark the block bad in NVS and continue in the next good block.
- Blocks 0–7 and 1020–1023 are factory-guaranteed valid: use them for the session table / write-pointer mirror (with a monotonic sequence number for recovery), never for sample data.
- **Pre-erase ahead:** while writing block N, erase block N+1 (skipping bad blocks) so tBE never stalls a flush.
- **Wear rotation:** store the last session's end block in NVS; start the next session at the next block, wrapping at 1019. (Endurance is not a real constraint — 60k cycles ÷ ~0.54 device-writes per match ≈ 1.1 × 10⁵ matches — but rotation is free.)
- On boot after unclean shutdown: scan the session table, find the last valid header, treat the last written page before a blank page as end-of-data. No stored write pointer is required for recovery.

### Record types in the log
| Type | Payload | Frequency |
|---|---|---|
| `SESSION_HEADER` | start UTC, ODRs, FS, firmware, device ID, ADXL372 config | 1 per session |
| `IMU_ROWS` | N × 12 B packed int16 (ax ay az gx gy gz), implicit index | continuous |
| `IMPACT_EVENT` | imu_sample_index, esp_timer µs, pre-trigger count, peak, N × 6 B ADXL372 XYZ | per impact |
| `MARKER` | sync-LED flash index, button press | sparse |
| `CONFIG` | full settings struct + sequence number + sample index at which it took effect | at session start and on every in-session change |
| `SESSION_TRAILER` | sample count, impact count, saturation count, CRC, upload state | 1 per session |

### Session lifecycle
`idle → start session (write header page) → capture → stop (write trailer) → upload → erase session blocks`. Never erase before upload is acknowledged by the backend.

---

## 7. Connectivity and upload

- **During a session: WiFi in modem-sleep or off by default.** Streaming as the *only* copy was rejected for reliability; local logging is the fallback-proof path and cuts average current from ~170 mA to ~60 mA.
- **Optional live mirror (phase 2, `stream_mirror` flag, default off, core 0 only):** after the writer commits a buffer to NAND, page copies (2 KB + session ID + page index) go to a TX ring on core 0 and are sent in bursts (~every 32 pages) over the existing HTTP(S) path when RSSI is good, with modem-sleep between bursts. Rules: capture and the NAND writer never wait on the network; if the TX ring is full, pages are dropped from the stream, never from the log; NAND stays authoritative and ack-before-erase applies to the complete page set; the post-session upload becomes a delta of pages the backend did not receive. Budget: +20–40 mA average in bursty mode (~7–8 h runtime), +~128 KB PSRAM.
- **After a session:** connect, upload the binary log in batches over HTTP(S) to the existing Flask backend (reuse the proven batch-upload/retry logic from the C3 generation), wait for ack, then erase. Upload format: session header + raw records, optionally gzip.
- Backend performs impact detection, video sync, and shot-type classification (Random Forest first, then LightGBM; impact-intensity features from the ADXL372 packets). The firmware does no classification.
- Keep the existing ping/command channel: the backend can send commands back in ping responses.

---

## 7a. Internal flash and settings policy (hard constraint)

The ESP32-S3's own 4 MB flash may be **read** at any time but must **never be programmed or erased while a session is active**: every internal-flash write/erase disables the cache and halts both cores via IPC for up to tens of ms, which eats directly into the IMU FIFO margin.

| | Internal flash | NAND |
|---|---|---|
| Firmware, OTA slots, bootloader, UI assets | ✓ (read-only during session) | — |
| Config, calibration matrices, credentials (NVS) | read during session; **written only in idle** | — |
| Session data, `CONFIG`/`MARKER` records, session table, write pointer, bad-block updates | — | ✓ during session |

Rules:
- `esp_wifi_set_storage(WIFI_STORAGE_RAM)`; `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=n`; OTA requests are refused while `session_active`.
- **Settings changed during a session** apply immediately to the in-RAM settings struct (the single source of truth) and are appended to the NAND log as a `CONFIG` record carrying the sample index at which they took effect and a monotonic sequence number. NVS is not touched.
- **At session stop, in this order:** write `SESSION_TRAILER` → stop capture → commit the settings struct to NVS → (later) upload.
- **Boot reconciliation:** the recovery scan reads the last `CONFIG` record of the last session; if its sequence number is newer than the NVS copy, apply it and commit to NVS before anything else. This is the only path by which a mid-session change survives a crash or battery loss.
- No module other than `settings.c` may call `nvs_commit()`; it asserts `!session_active`.

---

## 8. Power budget (700 mAh, 120 min target)

| Item | Current |
|---|---|
| ESP32-S3 active, RF off | 30–50 mA |
| LP5810 + LED (on) | 0.4 mA + LED current |
| IMU combo high-perf + HAODR | ~0.6 mA |
| ADXL372 at 3.2 kHz | 0.02 mA |
| WiFi modem-sleep | ~15 mA |
| NAND (25 mA, <1 % duty) | ~0.1 mA avg |
| LED (if on) | 2–5 mA |
| **Average** | **~50–75 mA → ~10–12 h runtime** |

Continuous WiFi TX would add ~130 mA and cut this to ~4 h. Not acceptable as the default.

---

## 9. Deliverables expected from the rewrite

1. `spi_bus.cpp/h` — SPI2 (NAND) and SPI3 (sensors) init with DMA, ISR affinity core 1, device handles, async transaction helpers
2. `nand_w25n01kv.cpp/h` — driver per §6 including bad-block table, ECC/P-FAIL/E-FAIL handling, pre-erase, session table
3. `imu_lsm6dsv32x_platform.cpp/h` — `platform_read/write/delay` for the ST driver over `spi_master` DMA; HAODR set-2 enable in power-down, then FIFO config for 800/800 Hz batching with watermark on INT1
4. `hig_adxl372.cpp/h` — init in standby, ODR 3200 / BW 1600, threshold, triggered FIFO with pre/post split, INT1 handler, one-burst FIFO read, re-arm
5. `capture.cpp/h` — static buffers, ISRs, sampling/impact/writer tasks, atomic swap, typed record writer, session start/stop
6. `power.cpp/h` — BQ25180 I2C (STAT0), VBAT ADC, LED gating, modem-sleep control
6a. `settings.c/h` — in-RAM settings struct, `CONFIG` record emitter, idle-only NVS commit with `session_active` assert, boot reconciliation against the last NAND `CONFIG` record
7. `upload.cpp/h` — post-session batch upload with retry, ack-before-erase
8. A bring-up test sketch that: reads NAND JEDEC ID, clears BP bits, erases/programs/reads one block with ECC check; reads IMU WHO_AM_I, enables HAODR set 2 and verifies the measured ODR against a 1 s hardware timer; reads ADXL372 DEVID (0xAD/0x1D/0xFA — **VERIFY**), fires a test impact by tapping and dumps the packet; runs 60 s of capture with WiFi associated and reports IMU FIFO overruns (must be 0), impact packets received, and free PSRAM.

9. A calibration sketch (run once per board, results stored in NVS): six static orientations (±X, ±Y, ±Z up) must each read +1 g on the expected axis for **both** accelerometers; hand-rotation about each axis must give the right-hand-rule sign on the gyro; a frame tap must appear on the same axis with the same sign on both accelerometers. Output: two rotation matrices (+ mirror flags) → NVS → `SESSION_HEADER`.

Keep every module free of dynamic allocation after `setup()`. Provide `#define`s for all pins and sizes at the top of one config header.

---

## 10. Open items — resolve before or during bring-up

- **VERIFY** LSM6DSV32X pinout vs DSO32 footprint (schematic symbol U8)
- **VERIFY** LSM6DSV32X WHO_AM_I value and physical FIFO depth
- **VERIFY** ADXL372 pinout for the LGA-16 footprint, DEVID registers, and LCSC availability/price
- **VERIFY** arduino-esp32 / ESP-IDF version → PSRAM DMA alignment rule
- **VERIFY** free PSRAM with WiFi connected before fixing buffer size
- Tune the ADXL372 trigger threshold from the first real sessions: it must not fire on swing loads (20–40 g) and must not miss soft touches (chiquita, globo) — if soft shots fall under the threshold, they are still fully captured by the IMU stream; the packet is a bonus, not a requirement.
- Decide session start/stop trigger (button press pattern via BQ25180 /INT, backend command, or motion-based auto-start).
