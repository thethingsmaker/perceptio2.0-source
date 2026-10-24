# PADELIO 4.0 — Securing the Platform: Step-by-Step Procedure

Target: ESP32-S3-MINI-1-N4R2, ESP-IDF v6.x native, USB-Serial-JTAG on IO19/IO20 (pogo D±).
Scope: make shipped units unreadable, unmodifiable, non-impersonatable, while keeping a factory/RMA recovery path.

**Every eFuse burn below is permanent. Read each phase fully before running it. Do Phase 0–2 on scrap boards first.**

Command names follow ESP-IDF's tools (`espsecure.py`, `espefuse.py`, `idf.py`). On IDF 6.x the same operations are also exposed as `idf.py secure-*` / `idf.py efuse-*`; either form is valid. Replace `PORT` with the device's USB-Serial-JTAG port.

---

## Phase 0 — Prerequisites (once)

1. **Signing machine:** an offline (air-gapped or HSM-backed) machine holding the Secure Boot private key. It never touches the network or the assembler. Backups: two encrypted copies in separate locations.
2. **Two build profiles in the repo:**
   - `sdkconfig.defaults` — development: no secure boot, no flash encryption, JTAG on.
   - `sdkconfig.defaults.release` — everything in Phase 2 enabled. Build with `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release"`.
3. **Partition table with encryption flags** (`partitions.csv`):
   ```
   # Name,     Type, SubType,  Offset,   Size,    Flags
   nvs_key,    data, nvs_keys, ,         0x1000,  encrypted
   nvs,        data, nvs,      ,         0x6000,
   otadata,    data, ota,      ,         0x2000,  encrypted
   phy_init,   data, phy,      ,         0x1000,  encrypted
   ota_0,      app,  ota_0,    ,         0x180000,
   ota_1,      app,  ota_1,    ,         0x180000,
   assets,     data, spiffs,   ,         0x80000,
   ```
   App partitions, bootloader and partition table are always encrypted when flash encryption is on; `nvs` is encrypted via NVS encryption (Phase 2.3), not by the `encrypted` flag.
4. **Per-device record in the backend:** serial (chip MAC), Secure Boot key ID, device HMAC secret (Phase 3), eFuse summary hash, firmware version at provisioning.
5. Keep **one unlocked board per production batch** in the lab.

---

## Phase 1 — Validate on scrap boards (Flash Encryption, Development mode)

Purpose: prove the encrypted partition layout, NVS encryption and OTA work before anything irreversible is done in Release mode. Development mode burns eFuses too (the flash key), but leaves readback/re-flash possible; these boards are not shippable.

1. `idf.py menuconfig` → *Security features*:
   - `Enable flash encryption on boot` → **Development mode**
   - `Enable NVS encryption` = y (flash-encryption-based scheme)
2. `idf.py build flash monitor` — the bootloader generates the key, burns it, encrypts the flash on first boot (takes ~1 min), reboots.
3. Verify: `idf.py efuse-summary` shows `SPI_BOOT_CRYPT_CNT = 1`, `BLOCK_KEYx purpose = XTS_AES_128_KEY`.
4. Re-flash the app with `idf.py encrypted-flash` (plain `flash` no longer works). Confirm NVS read/write, OTA to the other slot, settings reconciliation (§7a of the firmware brief).
5. Confirm the capture path is unaffected: run a 60 s session with WiFi active — flash encryption adds nothing to runtime on the S3 (hardware XTS-AES in the cache path), but verify FIFO overruns = 0 anyway.

---

## Phase 2 — Release configuration (production boards)

### 2.1 Generate the Secure Boot signing key (signing machine, once per product line)
```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
```
ESP32-S3 Secure Boot V2 uses RSA-3072 (ECDSA schemes are for the C2/C6/H2/P4 line). Export the public key digest for the record:
```
espsecure.py digest_sbv2_public_key --keyfile secure_boot_signing_key.pem --output sb_digest.bin
```

### 2.2 `sdkconfig.defaults.release`
```
# Flash encryption — Release
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_SECURE_FLASH_ENCRYPTION_AES256=y          # XTS-AES-256 (S3 supports 128 and 256)

# Secure Boot V2
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_BOOT_V2_RSA_ENABLED=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"   # only present on the signing machine
CONFIG_SECURE_BOOT_ALLOW_JTAG=n                  # bootloader burns HARD_DIS_JTAG + DIS_USB_JTAG on first boot
CONFIG_SECURE_BOOT_ALLOW_ROM_BASIC=n
CONFIG_SECURE_BOOT_ALLOW_SHORT_APP_PARTITION=n

# ROM download mode → Secure Download Mode (keeps RMA path, blocks readback)
CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y
# CONFIG_SECURE_DISABLE_ROM_DL_MODE=y            # only if you accept that a bricked unit is scrap

# Anti-rollback
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
CONFIG_BOOTLOADER_APP_SECURE_VERSION=1
CONFIG_BOOTLOADER_APP_SEC_VER_SIZE_EFUSE_FIELD=16

# NVS encryption
CONFIG_NVS_ENCRYPTION=y

# No console/debug leaks
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y             # keep for field logs, or set NONE for silent units
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=n
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
```

### 2.3 Build the signed images (signing machine)
```
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" set-target esp32s3
idf.py build
```
Outputs: `build/bootloader/bootloader.bin` (signed), `build/partition_table/partition-table.bin`, `build/padelio.bin` (signed). Ship these three files to the provisioning fixture; **the `.pem` never leaves the signing machine.**

For later OTA images, sign on the signing machine with the same key:
```
espsecure.py sign_data --version 2 --keyfile secure_boot_signing_key.pem --output padelio_signed.bin padelio.bin
```
Increment `CONFIG_BOOTLOADER_APP_SECURE_VERSION` for every release that fixes a security issue (anti-rollback counter, 16 steps available).

---

## Phase 3 — Per-device provisioning (fixture at the assembler, one script)

Run on a fresh, never-booted module. Order matters.

1. **Read identity**
   ```
   esptool.py --port PORT chip_id           # record MAC → serial
   espefuse.py --port PORT summary          # must be all-default; abort otherwise
   ```
2. **Burn the device HMAC secret** (per-device 32-byte random key generated on the fixture, stored in the backend against the serial, then discarded from the fixture):
   ```
   espefuse.py --port PORT burn_key BLOCK_KEY5 device_hmac_key.bin HMAC_UP
   ```
   Firmware uses `esp_hmac_calculate(HMAC_KEY5, challenge, …)` to answer backend challenges. Read-protection on the block is applied automatically for HMAC purposes.
3. **Flash the signed images (plaintext; the bootloader encrypts on first boot)**
   ```
   esptool.py --port PORT --chip esp32s3 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 padelio.bin
   ```
   (Offsets from your `partitions.csv`; on the S3 the bootloader is at 0x0.)
4. **First boot** — power-cycle. The bootloader, in this order: burns the Secure Boot key digest and `SECURE_BOOT_EN`; generates and burns the flash-encryption key; encrypts bootloader, partition table, app, and flagged partitions; burns `SPI_BOOT_CRYPT_CNT`, `DIS_DOWNLOAD_MANUAL_ENCRYPT`, `HARD_DIS_JTAG`, `DIS_USB_JTAG`, `ENABLE_SECURITY_DOWNLOAD`, `DIS_DIRECT_BOOT`; reboots into the encrypted, verified app. Watch the console: it must end with the app's own banner. Allow ~90 s.
5. **Optional extra hardening (manual, after step 4 succeeds)**
   ```
   espefuse.py --port PORT burn_efuse DIS_USB_OTG_DOWNLOAD_MODE 1
   espefuse.py --port PORT burn_efuse DIS_USB_SERIAL_JTAG 1     # only if console over USB is not wanted in the field; UART pads then remain the RMA path
   espefuse.py --port PORT burn_efuse SECURE_BOOT_AGGRESSIVE_REVOKE 1
   ```
   Note: `DIS_USB_SERIAL_JTAG` also removes the console and the auto-reset flashing path; with Secure Download Mode the UART0 pads (RXD0/TXD0 test pads) still allow flashing signed images.
6. **Lock the eFuse blocks** (last):
   ```
   espefuse.py --port PORT write_protect_efuse SECURE_BOOT_EN
   espefuse.py --port PORT write_protect_efuse DIS_DOWNLOAD_MODE ENABLE_SECURITY_DOWNLOAD HARD_DIS_JTAG DIS_USB_JTAG
   ```
7. **Verify and record**
   ```
   espefuse.py --port PORT summary --format json > efuse_<serial>.json
   ```
   Fixture checks: `SECURE_BOOT_EN=1`, `SPI_BOOT_CRYPT_CNT` odd, key digest matches `sb_digest.bin`, `HARD_DIS_JTAG=1`, `ENABLE_SECURITY_DOWNLOAD=1`, HMAC key block purpose `HMAC_UP`. Any mismatch → quarantine the unit.
8. **Functional test** on the fixture: BLE/WiFi association, backend challenge–response with the device key, 30 s capture, LED and button. Then power off, label, pack.

---

## Phase 4 — Application-side items (firmware brief additions)

- **Device authentication:** every upload and command channel authenticates with an HMAC over a backend nonce using the eFuse key (`esp_hmac_calculate`). No shared secrets in firmware.
- **OTA:** `esp_https_ota` with server certificate pinning (`esp_crt_bundle` or a pinned CA in the encrypted app), signed images only (Secure Boot enforces this), anti-rollback check before switching slots.
- **NAND data:** not covered by flash encryption. If session data must be confidential, AES-CTR each 2 KB page on core 1 with a key derived via `esp_hmac_calculate` from a per-session nonce; store the nonce in `SESSION_HEADER`. ~10 KB/s, negligible CPU. If IMU data is considered low-sensitivity, document the decision instead.
- **Debug surfaces:** no `esp_console` commands in release builds; log level WARN; no memory-dump or register-dump commands reachable over WiFi or USB.
- **Secure Download Mode awareness:** after Phase 3 a unit can be re-flashed with signed images over UART0/USB (basic write commands only). Keep the flashing procedure in the RMA runbook; it needs the current signed image set, nothing else.

---

## Phase 5 — RMA / recovery runbook

| Situation | Action |
|---|---|
| App wedged, Secure Download Mode enabled, USB-Serial-JTAG enabled | hold IO0 low at reset via the pad → `esptool.py write_flash` the signed image set → reboot |
| `DIS_USB_SERIAL_JTAG` burned | same via UART0 pads (RXD0/TXD0/GND/EN/IO0), 3V3 logic |
| `DIS_DOWNLOAD_MODE` burned | no recovery; unit is scrap — this is why Secure Download Mode is the recommended setting |
| Secure Boot key compromised | rotate: sign new images with key #2, push OTA that burns revocation of digest #1 (`SECURE_BOOT_KEY_REVOKE0`); units that never OTA'd remain on key #1 |
| Flash-encryption key | device-unique, never leaves the chip, cannot be rotated; not a fleet-wide risk |

---

## Checklist before the first production batch

- [ ] Phase 1 passed on ≥3 scrap boards, including OTA and settings reconciliation
- [ ] Release build reproducible from a tagged commit; hashes of the three binaries recorded
- [ ] Signing key backed up (2 encrypted copies), digest recorded
- [ ] Provisioning script tested end-to-end on ≥3 boards; eFuse JSON archived per serial
- [ ] Backend challenge–response verified against a provisioned board
- [ ] RMA runbook tested: recover a provisioned board via Secure Download Mode
- [ ] One unlocked board per batch reserved and labelled
