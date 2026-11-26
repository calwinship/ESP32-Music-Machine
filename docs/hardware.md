# Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — hardware reference

Compiled 2026-11-14 from the vendor files. Every fact cites its source. `docs/vendor/` is gitignored: re-create it with
`tools/fetch_vendor.sh` (checksums in `docs/ref/vendor-files.sha256`).
**UNVERIFIED** = not confirmed by a vendor source file; do not rely on it without checking.

## Sources and abbreviations

| Tag | Path / origin |
|---|---|
| `DEMO` | `docs/vendor/Demo/ESP32-S3-Knob-Touch-LCD-1.8-Demo/ESP-IDF/` (zip dated 2025-06-20) |
| `ARD` | `docs/vendor/Demo/ESP32-S3-Knob-Touch-LCD-1.8-Demo/Arduino/examples/` |
| `SCH p1..p5` | `docs/ref/schematic/` (committed copy of the schematic zip's PNGs) — p1 `1_LCD&POWER.png`, p2 `2_ESP32S3-R8.png`, p3 `3_ESP32-CHIP.png`, p4 `4_OTHER.png`, p5 `5_DAC.png` |
| `WIKI` | `docs/ref/waveshare-wiki.txt` (committed text copy of the wiki page, fetched 2026-11-14) |
| `BIN-S3` | `docs/vendor/BIN/…/WX-ESP32S3-KNOB_V1.2.bin` — factory S3 image. Evidence from `strings`; weaker than source |
| `BIN-32` | `docs/vendor/BIN/…/ESP32-KNOB_ESP32_0.bin` — factory ESP32-U4WDH image. Same caveat |
| datasheets | `docs/vendor/datasheets/Pcm5100a.pdf`, `Esp32_datasheet_en.pdf` (downloaded from the links on the WIKI page) |

Extraction note: 3 PDFs with GBK-encoded (Chinese) filenames in `DEMO/03_DRV2605_Test/components/SensorLib/datasheet/` could not be extracted on macOS. They are third-party datasheets and were not used here.

## Chips and ownership

| Peripheral | Part | Owner | Interface | Source |
|---|---|---|---|---|
| Display 1.8" round, 360×360 | **ST77916** (see note) | ESP32-S3 | QSPI, SPI2_HOST | `DEMO/08_LVGL_Test/main/user_config.h:5,15-26`; SCH p2, p1 (U5) |
| Touch | **CST816** (wiki datasheet link: CST816D; BIN-S3 string: `CST816S`) | ESP32-S3 | I2C0 addr 0x15 | `DEMO/08_LVGL_Test/main/user_config.h:28`; WIKI datasheet link; BIN-S3 |
| Rotary encoder #1 (SW2, SSCM110100) | — | ESP32-S3 | 2 GPIO | SCH p1, p2 |
| Rotary encoder #2 (SW1, SSCM110100) | — | ESP32-U4WDH | 2 GPIO | SCH p1, p3 |
| TF / micro-SD (CARD1, TF-018) | — | ESP32-S3 | SDMMC 4-bit | SCH p2, p4; `DEMO/02_SD_Card/components/sdcard_bsp/sdcard_bsp.c:15-20,50` |
| Audio DAC | PCM5100APWR (U12) | **Shared**, muxed by CH445P (U18) | I2S (no MCLK) | SCH p5; `DEMO/07_Audio_Test/components/audio_bsp/audio_bsp.c:66,204` |
| Microphone | MSM261D4030H1CPM (MIC1), PDM, L/R tied GND | ESP32-S3 | I2S0 PDM RX | SCH p4, p2; `DEMO/07_Audio_Test/components/audio_bsp/audio_bsp.c:84-94` |
| Haptic driver | DRV2605L (U13, "DRV2605LDGSR"), LRA motor on PP1/PP2 | ESP32-S3 | I2C0 addr 0x5A (same bus as touch) | SCH p5, p2; `DEMO/03_DRV2605_Test/main/user_config.h:10` |
| USB-UART bridge | U10, part number not printed. Enumerates as **WCH `1a86:7523` (CH340 family)**; the 10-pin pinout matches CH340K, but the exact suffix is UNVERIFIED. macOS's built-in driver gives `/dev/cu.usbserial-*`, so no driver install is needed | ESP32-U4WDH UART0 | USB | SCH p3; USB enumeration 2026-11-14 |
| SPI flash for S3 | Schematic: W25Q128JVPIQ (U3), 16 MB. **This board reports JEDEC manufacturer 0x68, device 0x4018** (Winbond is 0xEF), so the part is likely substituted. Size 16 MB is confirmed | ESP32-S3 | SPI0/1 | SCH p2; WIKI "16MB Flash"; `esptool flash_id` 2026-11-14 |
| PSRAM | 8 MB octal, inside ESP32-S3R8 | ESP32-S3 | — | WIKI; `DEMO/08_LVGL_Test/sdkconfig.defaults` `CONFIG_SPIRAM_MODE_OCT=y` |
| 3V3 regulator | TLV62569DBVT (U19), 5V→3V3 | — | — | SCH p1 |
| DAC supply | SGM2036-3.3 LDO (U20), 5V→`3V3_DAC` | — | — | SCH p5 |

**Display controller note.** No source file names the ST77916. The ESP-IDF demo drives the panel through Espressif's
`esp_lcd_sh8601` QSPI driver and replaces its init sequence with a custom table (`DEMO/08_LVGL_Test/main/main.c:32-222,386-407`).
BIN-S3 contains `/lib/ESP32_Display_Panel-0.2.2/src/lcd/base/esp_lcd_st77916.c` and `no mem for st77916 panel`, so the factory
firmware uses an ST77916 driver. The schematic labels only the 28-pin FPC (U5, SCH p1), not the controller.
Status: **ST77916, UNVERIFIED (binary evidence only)**.

## ESP32-S3 GPIO map

Net names come from the net table on SCH p2. Every S3 GPIO that the schematic exposes is used, so there are **no spare GPIOs**.

| GPIO | Net | Function / notes | Source |
|---|---|---|---|
| 0 | `I2S_SWITCH_IN` | CH445P `IN`: selects which chip drives the DAC. 10 kΩ pull-up R58. Also on CN1 pin 8. The demo drives it **high to give the DAC to the S3**. Strapping pin. | SCH p2, p5, p1; `DEMO/07_Audio_Test/components/audio_bsp/audio_bsp.c:197-204` |
| 1 | `BATT_ADC` | ADC1 ch0. 10k/10k divider R62/R63 **from net `5V`** (÷2; demo and our code multiply by 2). Measured: **on battery the net is the cell** (4.02–4.06 V), on USB it is the USB rail (4.66–4.78 V), so the cell's charge can be read only while unplugged. | SCH p4, p2; `DEMO/01_ADC_Test/components/adc_bsp/adc_bsp.c:32,46` |
| 2 | `SDMMC_D3` | SD DAT3 (also CD). 10 kΩ pull-up | SCH p2, p4; `sdcard_bsp.c:19` |
| 3 | `SDMMC_CMD` | SD CMD, 10 kΩ pull-up | SCH p2, p4; `sdcard_bsp.c:15` |
| 4 | `SDMMC_SCK` | SD CLK | SCH p2, p4; `sdcard_bsp.c:20` |
| 5 | `SDMMC_D0` | SD DAT0, 10 kΩ pull-up | SCH p2, p4; `sdcard_bsp.c:16` |
| 6 | `SDMMC_D1` | SD DAT1, 10 kΩ pull-up | SCH p2, p4; `sdcard_bsp.c:17` |
| 7 | `EC1_B` | Encoder #1 B, 10 kΩ pull-up R59 | SCH p2, p1; `DEMO/04_Encoder_Test/main/user_config.h:12` |
| 8 | `EC1_A` | Encoder #1 A, 10 kΩ pull-up R60 | SCH p2, p1; `DEMO/04_Encoder_Test/main/user_config.h:11` |
| 9 | `TP_INT` | Touch interrupt. Defined but **not used** by the IDF demo, which polls | SCH p2; `DEMO/08_LVGL_Test/main/user_config.h:30` |
| 10 | `TP_RST` | Touch reset. Defined but **never driven** by the IDF demo | SCH p2; `user_config.h:29` |
| 11 | `TP_SDA` = `HAPTIC_SDA` | I2C0 SDA, 5.1 kΩ pull-up R5 | SCH p2; `user_config.h:10` |
| 12 | `TP_SCL` = `HAPTIC_SCL` | I2C0 SCL, 5.1 kΩ pull-up R6. Demo runs at 300 kHz | SCH p2; `DEMO/08_LVGL_Test/components/i2c_bsp/i2c_bsp.c:34` |
| 13 | `LCD_QSPI_SCL` | LCD QSPI clock | SCH p2; `user_config.h:20` |
| 14 | `LCD_QSPI_CS` | LCD chip select | SCH p2; `user_config.h:19` |
| 15–18 | `LCD_QSPI_D0..D3` | LCD QSPI data 0–3 | SCH p2; `user_config.h:21-24` |
| 19 / 20 | `USB_DN` / `USB_DP` | Native USB (USB-Serial-JTAG), to CN1 pins 3/4 | SCH p2, p1 |
| 21 | `LCD_RST` | LCD reset | SCH p2; `user_config.h:25` |
| 38 | `ESP32S3_TX` | Inter-chip UART, to ESP32 IO18 | SCH p2, p3 |
| 39 | `S3_I2S_DAC_BCK` | I2S bit clock to the CH445P S2A input | SCH p2, p5; `DEMO/07_Audio_Test/main/user_config.h:15` |
| 40 | `S3_I2S_DAC_LRCK/WS` | I2S word select to CH445P S2C | SCH p2, p5; `user_config.h:16` |
| 41 | `S3_I2S_DAC_DIN` | I2S data out to CH445P S2B | SCH p2, p5; `user_config.h:17` |
| 42 | `SDMMC_D2` | SD DAT2, 10 kΩ pull-up R10 | SCH p2, p4; `sdcard_bsp.c:18` |
| 43 / 44 | `U0TX` / `U0RX` | UART0 pins are labelled, but no other page uses these nets. **Where they go: UNVERIFIED** | SCH p2 |
| 45 | `PDM_MIC_SCK` | PDM mic clock. Strapping pin | SCH p2, p4; `DEMO/07_Audio_Test/main/user_config.h:21` |
| 46 | `PDM_MIC_DATA` | PDM mic data. Strapping pin | SCH p2, p4; `user_config.h:20` |
| 47 | `LCD_BLK` | Backlight: gate of AO3400A low-side switch (Q1) with a 10 kΩ gate pull-down, so **active-high**. Demo uses LEDC 50 kHz, 8-bit; duty 255 = full | SCH p1, p2; `DEMO/08_LVGL_Test/components/lcd_bl_pwm_bsp/lcd_bl_pwm_bsp.c:21-38` |
| 48 | `ESP32S3_RX` | Inter-chip UART, to ESP32 IO23 | SCH p2, p3 |
| 26–32 | SPI flash | Used by the W25Q128 (SPICS0/SPICLK/SPID/SPIQ/SPIWP/SPIHD); do not use | SCH p2 |
| 33–37 | — | No net on SCH p2. Normally taken by octal PSRAM on S3R8: **UNVERIFIED (general ESP32-S3 knowledge)** | SCH p2 |

The strapping-pin remarks for GPIO0/45/46 are general ESP32-S3 knowledge. The datasheet is linked from WIKI but was not downloaded.
**UNVERIFIED.**

`LCD_TE` (display tearing-effect output) appears on U5 pin 23 (SCH p1). On SCH p2 it has a no-connect ✕ and is not tied to any
GPIO, so tearing-effect sync is not available (my reading of the drawing).

## ESP32-U4WDH GPIO map (do not flash without asking)

4 MB flash in package (WIKI "with 4MB Flash"; BIN-32 header flash size 4 MB).

| GPIO | Net | Function | Source |
|---|---|---|---|
| EN | `ESP32_EN` | 10 kΩ pull-up R54, 1 µF C22. Driven by U10 RTS# (auto-reset) | SCH p3 |
| 0 | `ESP32_IO0` | 10 kΩ pull-up R50. U10 DTR# (auto-boot) | SCH p3 |
| U0TXD (pin 41) / U0RXD (pin 40) | `ESP32_UART0_TX` / `_RX` | To U10 RXD/TXD. 470 Ω R65 in the TX line | SCH p3 |
| 18 | `ESP32S3_TX` | Inter-chip UART, to S3 GPIO38 | SCH p3 |
| 23 | `ESP32S3_RX` | Inter-chip UART, to S3 GPIO48 | SCH p3 |
| 19 / 22 | `EC2_A` / `EC2_B` | Encoder #2 (SW1), 10 kΩ pull-ups R55/R56 | SCH p3, p1 |
| 25 / 26 / 27 | `ESP32_I2S_DAC_BCK` / `_DIN` / `_LRCK/WS` | I2S to CH445P S1A / S1B / S1C | SCH p3, p5 |
| 32 | `XSMT` | **PCM5100A soft-mute pin.** Only the ESP32 controls DAC mute | SCH p3, p5 |
| 2, 4, 21, 33 | `ESP32_IO2/IO4/IO21/IO33` | Labelled on p3 but found on no other page. **UNVERIFIED** | SCH p3 |

Factory ESP32 firmware (BIN-32 app descriptor): project `TAIJI_KNOB_32`, built 2025-04-18 with IDF `v5.4-727-g5cbd2a3877`.
Its strings include Bluetooth A2DP sink/source, AVRC, `TAIJI_KNOB_HID`, `../main/uart1.c` (task `task_uart1`), `../main/iot_knob.c` and an I2S TX
channel. It appears to be a Bluetooth audio receiver plus BT HID knob that talks to the S3 over UART1. **What it actually does: UNVERIFIED.**

## How the two chips are linked

1. **UART:** S3 GPIO38 (`ESP32S3_TX`) ↔ ESP32 IO18, and S3 GPIO48 (`ESP32S3_RX`) ↔ ESP32 IO23 (SCH p2, p3).
   Direction is inferred only from net names: "S3_TX" suggests the S3 transmits on GPIO38. **Direction, baud rate and protocol are UNVERIFIED.**
   No ESP-IDF demo uses this link.
2. **Shared DAC via CH445P (U18)**, a 4-pole double-throw analog switch (SCH p5; WIKI "Four-pole double-throw"). The `S1x` inputs come
   from the ESP32 and the `S2x` inputs from the S3, while `Dx` outputs drive the PCM5100A BCK/DIN/LRCK. `IN` = S3 GPIO0, `EN#` = GND (always enabled).
   The demo comment says GPIO0 high → S3 (`audio_bsp.c:204`). R58 pulls GPIO0 up, so the S3 is selected while GPIO0 floats.
   Which level selects which side, per the CH445P datasheet: **UNVERIFIED** (datasheet not downloaded).
3. **DAC mute belongs to the ESP32:** `XSMT` = ESP32 IO32, pin 12 `32K_XP` (SCH p3, p5). See "XSMT (DAC mute) analysis" below.
   S3 audio depends on the ESP32 firmware driving IO32 high.
4. **USB:** connector CN1 (PM127-2-05-S-4.3, 10-pin, SCH p1) carries two USB pairs: S3 native USB (pins 3/4) and `USB_ESP32_DP/DN` to the
   U10 bridge (pins 5/6). CN1 also carries 5V, GND, S3 `GPIO0` (pin 8) and DAC `OUTR`/`OUTL` (pins 9/10). The Type-C socket is not in
   the schematic. WIKI: Type-C "Bidirectional Switchable for ESP32-S3/ESP32" and "pay attention to the Type-C direction".
   **Verified 2026-11-14:** one orientation enumerates the S3 (`303A:1001`, serial = S3 MAC); flipping the plug enumerates the ESP32's
   CH340 bridge (`1a86:7523`) instead. Which physical orientation is which is not recorded, so check the USB ID (`tools/monitor.py` refuses
   non-Espressif ports by default) before flashing. The board stays powered and running in either orientation.
5. **Encoders:** two physical switch parts, one per chip (WIKI "Dual encoder — Acting on ESP32S3 and ESP32 respectively"). Whether one knob
   turns both: **UNVERIFIED**.
6. **I2C bus:** S3 only. Touch (0x15) and DRV2605L (0x5A) share it. DRV2605L `EN` is tied to 3V3 and `IN/TRIG` to GND (SCH p2 table, p5), so
   playback must use internal trigger (the demo uses `MODE_INTTRIG`, `DEMO/03_DRV2605_Test/components/i2c_equipment/i2c_equipment.cpp`).

## XSMT (DAC mute) analysis

| Question | Answer | Source |
|---|---|---|
| External pull-up/down on XSMT? | **None.** The net connects only ESP32 pin 12 (IO32) and PCM5100A pin 17, with no resistor or capacitor on either page | SCH p3 (`ESP32_IO32`→`XSMT` label), SCH p5 (U12 pin 17) |
| Internal pull inside the PCM5100A? | **None documented.** Pin type is "Failsafe LVCMOS Schmitt trigger input"; Low = soft mute, High = soft un-mute. TI notes "where XSMT is not required, it can be directly connected to AVDD" | `docs/vendor/datasheets/Pcm5100a.pdf` (SLAS859), pin table note (1) and §9.3.3 |
| ESP32 IO32 state if firmware never touches it | **High-impedance, no pull.** IO_MUX pin 12 (32K_XP / GPIO32) is `oe=0, ie=0` both "At Reset" and "After Reset", with no `wpu`/`wpd` | `docs/vendor/datasheets/Esp32_datasheet_en.pdf` v4.9, Appendix A, Table IO_MUX (p. 69) |

**Conclusion:** if the ESP32 never drives IO32, XSMT **floats** and its logic level is **undefined**. The DAC could be muted or
un-muted, and could change with noise or leakage. The same applies while the ESP32 is in reset, in ROM download mode, or booting
before its app configures IO32. So no S3 audio result is meaningful unless the ESP32 firmware drives IO32 high. The factory
ESP32 firmware (BIN-32) contains I2S TX code, so it presumably manages XSMT, but **how it drives IO32 is UNVERIFIED**.
This is a reason never to erase the ESP32 without a plan. If S3 audio is ever silent, suspect XSMT first.

## Peripheral details

- **Display:** 360×360, RGB565 (`CONFIG_LV_COLOR_DEPTH=16` in `DEMO/08_LVGL_Test/sdkconfig`), `LV_COLOR_16_SWAP=y`. Flush areas are rounded to even
  coordinates (`main.c:261-275`). Draw buffers are 2 × (360 × 36) pixels in DMA RAM (`main.c:419-424`).
- **Encoder logic:** `bidi_switch_knob.c` treats A and B as **separate direction pulses**, not quadrature. A rising edge on A after
  debounce means `KNOB_RIGHT`, and on B means `KNOB_LEFT`. It polls every 3 ms with a 2-tick debounce and internal pull-ups
  (`DEMO/04_Encoder_Test/components/user_encoder_bsp/src/bidi_switch_knob.c:20-21,63-102,270-279`).
- **SD:** 4-bit SDMMC at `SDMMC_FREQ_HIGHSPEED`, FAT mounted at `/sdcard`, no card-detect pin (`sdcard_bsp.c:39-58`; SCH p4).
- **DAC sample rates without MCLK:** the PCM5100A PLL locks to BCK only at 16 (64fs only), 32, 44.1, 48 kHz and higher
  (`Pcm5100a.pdf` Table 11); there is no 22.05/24 kHz row. Verified by ear on this board: 32, 44.1 and 48 kHz (verification log).
- **DAC config pins (SCH p5):** FMT=GND, FLT=GND, DEMP=AGND, SCK=GND, so there is no MCLK. Output `OUTL`/`OUTR` → CN1 → 3.5 mm jack
  (WIKI lists a jack; its circuit is not in the schematic).
- **Audio demo:** I2S1 STD TX at 44.1 kHz/16-bit, PDM RX on I2S0 at 44.1 kHz mono (`audio_bsp.c:22-27,55,84`).
- **Power:** a battery socket, charger and power button are listed on WIKI but **not shown in these 5 schematic pages: UNVERIFIED**.

## Toolchain versions the demos target

The WIKI ESP-IDF section names no IDF version. Its Arduino section requires arduino-esp32 ≥ 3.2.0 and LVGL v8.4.0.

| Demo | Last saved with IDF (sdkconfig header) | `IDF_INIT_VERSION` |
|---|---|---|
| 01_ADC_Test, 02_SD_Card | 5.3.2 | 5.3.1 |
| 03_DRV2605_Test, 04_Encoder_Test | 5.4.0 | 5.3.1 |
| 05_WIFI_AP, 06_WIFI_STA, 07_Audio_Test | 5.4.0 | 5.4.0 |
| **08_LVGL_Test** | **5.3.2** | 5.4.0 |

- 08_LVGL_Test's `.vscode/settings.json` points at an `esp-idf v5.3.2` install, the strongest evidence for what it was last built with.
- **LVGL: 8.4.0**, pinned in `DEMO/08_LVGL_Test/main/idf_component.yml`.
- **`esp_lcd_sh8601`: version `'*'`, no `dependencies.lock` shipped.** When the demo was zipped (2025-06-20) the newest version was 1.0.0. Today `*`
  resolves to **2.0.1~1** (2.0.0 was released 2025-11-25), a major version bump that requires IDF ≥ 5.3. Source: Espressif component registry API, 2026-11-14.
- **Change made to reproduce the vendor build:** in the working copy `docs/vendor/work/08_LVGL_Test/main/idf_component.yml`,
  `esp_lcd_sh8601` is pinned from `'*'` to `'1.0.0'`. That is the only line changed; the vendor zip ships no `dependencies.lock`.
  Phase 1 build (IDF v5.3.2, 2026-11-14) resolved `esp_lcd_sh8601` 1.0.0, `cmake_utilities` 0.5.3, `lvgl` 8.4.0, with 0 compiler
  warnings (`docs/vendor/work/08_LVGL_Test/dependencies.lock`). `cmake_utilities` `0.*` resolves to 0.5.3 both today and in
  June 2025 (0.5.3 is from 2023-11-24), so it needed no pin.
- Every demo targets `esp32s3`. No ESP-IDF demo targets the ESP32-U4WDH.
- Factory S3 firmware is Arduino-based, not built from these demos (BIN-S3 descriptor: project `arduino-lib-builder`, IDF `v5.1.4`).

## Flash, partitions and console (08_LVGL_Test)

- Flash size: `sdkconfig.defaults` says 8 MB, but the committed `sdkconfig` says **16 MB**, which matches the W25Q128 and the BIN-S3 header.
  `sdkconfig` takes precedence when it exists.
- `partitions.csv`: nvs 0x6000, phy_init 0x1000, **factory app 8 MB**. (Our firmware/s3 now uses its own layout: see `firmware/s3/partitions.csv`.)
- Console: primary UART0 at 115200 (`CONFIG_ESP_CONSOLE_UART_DEFAULT`) **plus secondary USB-Serial-JTAG**
  (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`). UART0 has no visible route (see GPIO43/44). **Verified 2026-11-14:** boot and app
  logs arrive on the native-USB port (`/dev/cu.usbmodem101`, 303A:1001). The same console setting appears in all 8 demos.
- Restore image: flashing the S3 overwrites the factory app. **Restore from our verified backup** (`docs/backups.md`), not `BIN-S3`:
  `WX-ESP32S3-KNOB_V1.2.bin` differs from this board's shipped firmware in 510 of its 523 4 KB blocks.

## Hardware verification log

| Date | What | Evidence |
|---|---|---|
| 2026-11-14 | Chip enumerated on Type-C is the ESP32-S3. The first attempt showed no USB device at all; a different cable fixed it (user report), so the first cable was probably charge-only: QFN56 rev v0.2, 8 MB embedded PSRAM, 16 MB flash, MAC 44:bd:8d:7a:a6:50, USB 303A:1001 | `esptool chip_id` / `flash_id` |
| 2026-11-14 | S3 flash chip detected as **Boya** (JEDEC 0x68/0x4018), not Winbond as drawn | IDF boot log `spi_flash: detected chip: boya` |
| 2026-11-14 | Octal PSRAM 8 MB, AP gen 3, memory test OK | IDF boot log |
| 2026-11-14 | 08_LVGL_Test (vendor, `esp_lcd_sh8601` 1.0.0) boots on IDF v5.3.2; CST816 ACKs at 0x15 (`ESP_ERROR_CHECK` in `lcd_touch_init` passed) | boot log via `tools/monitor.py` |
| 2026-11-14 | Display shows the LVGL widgets demo, upright, correct colours; touch switches tabs | user confirmed (yes to all) |
| 2026-11-14 | 07_Audio_Test (vendor, mic loopback) runs; encoder #1 events both directions (`main: vol:` 10→5→11→7) | boot + 180 s log via `tools/monitor.py` |
| 2026-11-14 | Loopback audible on 3.5 mm jack: knob clicks picked up by mic, louder clockwise / quieter anticlockwise. So mic → S3 → CH445P → PCM5100A → jack works, and XSMT was **un-muted** with the factory ESP32 firmware running | user report |
| 2026-11-14 | 07 playback variant (`docs/vendor/work/07_Audio_Playback`: `AudioMode=PlaybackMusicmode`, `canon.pcm` replaced by 30 s of the user's "Sporting Paddy", 44.1 kHz s16 stereo) | user: "it sounds perfect" |

| 2026-11-14 | Plug flipped → ESP32 bridge `1a86:7523` at `/dev/cu.usbserial-10`; flipped back → S3 again (port only listed, never opened) | USB watcher log; user did the flips |
| 2026-11-14 | `tools/monitor.py` survives unplug/replug: 2 port losses, 2 reconnects, and it ignored the CH340 port while flipped. Whether the S3 kept running while unplugged is **unknown**: that build logs nothing after boot, and boot output sent before the port reopens can be lost | `monitor.py 150 --reset none` |
| 2026-11-14 | **IDF v5.5.6** (CMake 3.30.2 from idf_tools): vendor 08 and 07 rebuilt unmodified (same pin; lock: sh8601 1.0.0, lvgl 8.4.0, cmake_utilities 0.5.3). The only new warnings are 2 Kconfig notices from the 5.3-era sdkconfig. 08 boots with the same log sequence | build logs, `monitor.py` |
| 2026-11-14 | v5.5.6 08: widgets demo shows correctly; touch switches tabs | user: "yes to both" |
| 2026-11-14 | v5.5.6 07 loopback: knob clicks audible, louder clockwise; log `vol:` 10→19→12→19→14 | user: "yes to all" + 120 s log |
| 2026-11-14 | **firmware/s3 (own app, IDF v5.5.6)**: boots; display + touch init OK; knob decoded through `logic/knob` (log `value=51..53 (knob cw)`, `..49 (knob ccw)`) | `./flash` boot log |
| 2026-11-14 | firmware/s3 UI: arc + number shown; clockwise up / anticlockwise down; tap resets to 50; clean rendering | user: "yes to all" |
| 2026-11-15 | **DAC at 48 kHz, no MCLK** (PLL from BCK, `Pcm5100a.pdf` Table 11). `docs/vendor/work/07_Audio_Rate48`: copy of the 07 playback variant, only change `I2S_SAMPLE_RATE` 44100→48000 + clip (Sporting Paddy 0:10–0:40) resampled to 48 kHz s16 stereo; vendor left-justified format and volume 10 kept; IDF v5.5.6. Clean boot, `main: audio start` | user: normal speed and pitch, no crackle/dropouts |
| 2026-11-15 | **DAC at 32 kHz, no MCLK**: same test, `07_Audio_Rate32` (32000, clip resampled to 32 kHz) | user: normal speed and pitch, no crackle/dropouts |
| 2026-11-15 | **firmware/s3 partition table v2**: nvs 0x9000/24K, phy_init 0xf000/4K, factory 0x10000/3M, `tunes` data/fat 0x310000/0xCF0000 (13248K) readonly. Bootloader prints all 4 rows; app boots (`app: ready`). Format chosen for tunes: MP3 CBR 32 kbps mono, 32 kHz (user listening test; Opus 24 kbps rejected as "squeaky") | `./flash` boot log; user: UI "all as before" |
| 2026-11-15 | **`./sync-tunes` first write**: 16 tunes (56.0 min, 13.50 of 13.53 MB) as a fatfsgen raw FAT image to `tunes` via `parttool.py write_partition` (reads the table from the board): `Wrote 13565952 bytes at 0x00310000`, `Hash of data verified`. NVS 0x9000–0xEFFF MD5 identical before/after (`04930c8a…`); `esptool verify_flash` of bootloader, partition table and app: digest matched; app boots as before. Host checks: image parses back (IDF `fatfsparse.py`) identical to the staged files; seeking from the per-second index decodes bit-exact vs a full decode at 75/75 random positions (ffmpeg decoder), 0/25 without the bit-reservoir pre-roll | sync-tunes/esptool output, `./logs` |
| 2026-11-15 | **Tune player (firmware/s3)**: I2S1 **standard Philips** 16-bit stereo at 32 kHz (mono duplicated), fixed gain 0.1, GPIO0 high; read-only FAT mount of `tunes`; esp_audio_codec 2.6.2 MP3 decoder fed one 144-byte frame per call (consumed 144/144). Underrun self-test (300 ms with no data): 13 underruns, so the counter works. Full tune Clare Jig from its marker 0:06.80 to 3:23.52: `ended ... 5492 frames, underruns 0, decode errors 0`, position logged every second, two tap pause/resume cycles logged | `./logs 240`; user: no glitches, starts at the first note, title + time shown, tap pauses/resumes ("accept all") |
| 2026-11-15 | Loudness vs the vendor demo (left-justified, same 0.1 gain): user "seems quiet" but unsure whether quieter than the earlier clips. So the left-justified/I2S mismatch theory is **still UNVERIFIED**; Philips format itself plays correctly | user report |
| 2026-11-15 | **Player controls**: knob = +/-2 s per click (fast spins combined: +13, +16, -9 clicks logged; clamps at 0:00), seek while paused stays paused; long-press = volume mode (0-10, 3 dB steps; 6 -> 10 -> 4 logged) saved to NVS during playback with no underrun; swipe left/right = next/previous tune (starts at its first marker); tap = play/pause, never changed tune. NVS init kept the existing factory data (no erase). 5 min capture: 0 underruns | `./logs 300 --reset none`; user: "tests above passed" (clean jumps, label shown/hidden, swipes, taps) |
| 2026-11-15 | **Player UI round 2**: Latin-1 fonts (lv_font_conv 1.5.2; boot log `ui: fonts: Latin-1 (fadas)`), "Nóra Chríonna" shown with fadas; INDEX.BIN v2 (icon + colour per tune); per-tune icon + glow; ring colour per speed (teal/amber/coral); list button; list flick only scrolls (12 px tap slop); x/16 and play/pause icon removed; resume across a re-sync (`resuming tune id cb82b439 at 69481 ms (paused)`) | `./flash` boot log; user: "it looks great" |
| 2026-11-15 | **Boot fade-in**: backlight held off until the first tune screen is drawn, then a 180 ms fade (`screen on at 1561 ms after boot (tune shown)`); **tune list** of 64 px rows with centre snap and edge fade; tap plays the tapped tune | `./logs 300`; user: "the flash is gone", "all good" |
| 2026-11-15 | **BATT_ADC (GPIO1)** via `bsp_supply_mv` (ADC1 ch0, 12 dB, curve-fitting cal., x2): USB 4658–4784 mV (log, 17 reads); battery only 4.02–4.06 V (user reading the temporary on-screen value). So the `5V` net is the cell on battery | `./logs 300`; user report |
| 2026-11-15 | **Speed buttons, list button, battery icon**: tap a speed plays it from its start (13 taps logged, e.g. `speed: Slow 0:08.49 -> Medium 1:41.00 (playing)`); swipes do nothing on the main screen; battery icon shows a bolt on USB (`battery: on USB (4790 mV)`) and a level on battery; 0 underruns over the capture | `./logs 300`; user: "all good" (speed buttons slightly overlap the ring: next round) |
| 2026-11-15 | **Speed arc + paged tune list + guide**: speed scale is an arc (r=130) concentric with the ring; taps by angle (e.g. `+35.2 deg: speed 3 of 3`, `-41.8 deg: speed 1 of 3`); farthest top-row corner 163 px vs ring inner edge 170; tune list one page per tune in the tune-screen layout, fade-in-place on choose, swipe right back; guide on first start (`guide shown (first start)`, `guide marked as seen`); 0 underruns while scrolling during playback | `./logs 300` x2; user: "all good, swipe right closes it" |
| 2026-11-15 | **UI refinement**: one timeline ring (whole tune, one proportional segment per speed, played part bright; tap a segment: `ring tapped at 243 deg: speed 2 of 3`, `323 deg: speed 3 of 3`); speed name under the time; pause bars while paused; battery hidden on USB; pure black background, tune colour only on the icon; titles: `16 checked, 0 over 2 lines; farthest corner 134 px` ("Over the Water to Charlie" 256 px, 1 line); 0 underruns | `./flash` boot log, `./logs 300`; user: "all good" |
| 2026-11-15 | **micro-SD card is fitted** (as shipped): temporary card-level probe in `app_main` (uncommitted; SDMMC 4-bit, pins CLK 4/CMD 3/D0 5/D1 6/D2 42/D3 2, 400 kHz, no mount) printed `CARD FOUND: 480 MB`, `Name: APPSD`, `Type: SDSC`, CSD ver 1, capacity 983040 x 512 B, SSR bus_width=4. Filesystem not checked. Normal app reflashed afterwards, boots to `app: ready` | `./flash 15` boot log |
| 2026-11-16 | **SD card contents (read-only)**: MBR, partition 1 type 0x0C at sector 238, **FAT32** (VBR OEM `MSDOS5.0`, label `NO NAME`), 4 KB clusters, 476 MB total, **76 MB free**. Root: `System Volume Information/`, `aida64/`, `clockbg/`, `fonts/`, `mjpeg/`, `music/` (20 MP3s), `night7/`, `pic/`, `txt/`, `weather/`: vendor demo assets (MJPEG videos up to 99 MB, fonts, images). Mounted through a FatFs disk driver whose write refuses (status STA_PROTECT): 0 writes attempted. Temporary probe, uncommitted | `./flash 60` boot log |
| 2026-11-16 | **M4A/AAC decode on S3** (`esp_audio_codec` 2.6.2 simple decoder, type M4A, embedded clips, before the UI starts): moov-first mono 48 kHz 63 kbps (53.16 s) decoded **2551808 samples, same count as ffmpeg**, 21x realtime; moov-first stereo 48 kHz 128 kbps (30.02 s) **1440768 samples, same as ffmpeg**, 11.4x realtime. Original Voice Memo with **mdat before moov fails at byte 0** (`AUD_SDEC: Decoder not ready`); 4 of 16 recordings have that layout. Not played through the DAC | `./flash 60` boot log; ffmpeg/ffprobe on host |
| 2026-11-25 | **Listening test, MP3 bitrate** (mono 32 kHz, "Donald Blue" at 32/48/64/96 kbps as blind "Test A-D" on the board): user ranked them **96 > 64 > 48 > 32**, exactly in bitrate order, "but they all pretty much sound the same". Chosen for the SD card: **96 kbps**; flash stays 32 kbps (16 tunes fill it). All 4 bitrates played through the existing decoder (seeking not checked); tunes restored with `./sync-tunes` afterwards (16 tunes, hash verified) | `./logs` (`INDEX.BIN: 4 tunes`); user report |
| 2026-11-26 | **Tunes on the SD card**: `./sync-tunes --wipe-card` wrote a 41.61 MB FAT16 image (own builder, 1 KB clusters, 40464 clusters; 16 tunes at 96 kbps) through the firmware's USB link (sync mode: each 32 KB piece written, read back and compared on the board) in 140 s, 0.30 MB/s; a second run without `--wipe-card` 137 s. Boot: `SD card APPSD (480 MB) mounted at /tunes`, `INDEX.BIN: 16 tunes`. Before that, with the old FAT32 card: `SD card has no INDEX.BIN` and fallback to the `tunes` partition (16 tunes). Logs keep arriving after the console moves to the USB-Serial-JTAG driver. User: playback sounds right, knob seeking, speed segments and tune list work, "Syncing tunes" screen shown ("yes to all"). Underruns were not logged during the user's test (the 5-min capture ran before it) | `./sync-tunes` output, `./flash`/`./logs` boot logs, `tools/board_link.py` info; user report |
| 2026-11-26 | **Interrupted SD sync**: sync mode entered, `S` (sector 0 cleared), 20 of ~1270 pieces written, then the Mac side stopped. Board: `no sync traffic for 60 s: back to the player`, restart, SD mount fails (`mount_to_vfs failed`), **falls back to the `tunes` partition, 16 tunes**. Found and fixed: the failed mount discarded the card handle, so the board reported no card (`card_sectors=0`) and sync-tunes would have said "no SD card"; now it re-checks the card (`SD card: no tunes volume`, 983040 sectors). `./sync-tunes` then refused without `--wipe-card` (as designed) and with it rewrote the card in 137 s; boot: `SD card APPSD (480 MB) mounted`, 16 tunes | `./logs`, `tools/board_link.py` info, `./sync-tunes` output |

## Known quirks in the vendor demo code (for later)

- `08_LVGL_Test/main/main.c:6` includes the legacy `driver/i2c.h`, while `i2c_bsp.c` uses the new `i2c_master` API. That is harmless while no
  legacy function is called, but don't mix the two.
- `i2c_bsp.c` declares `drv2605_dev_handle` but 08 never adds the device, so it stays NULL in that demo.
- `lcd_touch_bsp.c:14` ignores the I2C read result.
- **Possible I2S format mismatch (UNVERIFIED):** `audio_bsp.c:64` uses `I2S_STD_MSB_SLOT_DEFAULT_CONFIG`, which is left-justified
  (`bit_shift = false` in IDF v5.3.2 `i2s_std.h`, ESP32-S3 branch). PCM5100A FMT is tied to GND, which means standard I2S
  (`Pcm5100a.pdf`, pin table and §9.3.x "FMT … Low for I2S"). If so, the DAC reads each sample one BCK late: about +6 dB, and wrap-around
  above half scale. The demo caps volume at 0.5, which would hide the wrap. "Sounds perfect" fits either way, so it neither confirms nor
  rules this out. Own code uses `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` (verified playing correctly 2026-11-15); the
  loudness comparison was inconclusive, so whether the vendor format was wrong is still open.

## Toolchain actually used (Phase 1)

- ESP-IDF v5.3.2 at `~/esp/v5.3.2/esp-idf`, Python env `idf5.3_py3.12_env` (Homebrew Python 3.12.16), esptool 4.13.0.
- **CMake is Homebrew's 4.5.0** (`/opt/homebrew/bin/cmake`); ninja 1.13.2 from Homebrew. On macOS, IDF lists CMake as optional (it recommends 3.30.2)
  and did not install it. Both demo builds and all the hardware results above used CMake 4.5.0.
