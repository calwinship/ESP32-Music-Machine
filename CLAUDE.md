# ESP32-S3-Knob-Touch-LCD-1.8 firmware

Waveshare dual-MCU board: **ESP32-S3R8** (our app) + **ESP32-U4WDH** (factory firmware, hands off).
**Hardware facts (pins, chip ownership, inter-chip links, verification log): `docs/hardware.md`.** Read it before touching hardware code.

## Commands (repo root)
- `./build`: build `firmware/s3`
- `./flash [secs]`: build + flash the **S3 only** + print boot logs (default 8 s)
- `./logs [secs] [--reset none]`: capture logs without a TTY (resets the board unless `--reset none`)
- `./test`: host unit tests: `components/logic` (cc + ASan/UBSan) and `tools/sync_tunes.py` (Python), no board needed
- `./sync-tunes [--no-flash] [--accept "<title>"] [--accept-all]`: `recordings/*.m4a` → MP3 + `INDEX.BIN` → FAT image →
  writes **only** the S3 `tunes` partition (~2–3 min). Refuses if it doesn't fit. Cache/image in gitignored `build-tunes/`
- `. tools/idf_env.sh`: ESP-IDF shell for manual `idf.py` use
- `tools/fetch_vendor.sh`: re-create the gitignored `docs/vendor/` (Waveshare demos, schematic, datasheets), checksum-verified

## Toolchain (pinned)
- **ESP-IDF v5.5.6** at `~/esp/v5.5.6/esp-idf`, CMake 3.30.2 (idf_tools), Homebrew Python 3.12 (3.14 breaks `export.sh`).
  Chosen 2026-11-14: last 5.x line, supported until ~Jan 2028; the vendor drivers run on it unmodified (verified on hardware).
  It is past its 12-month "in service" period; v6.x needs driver porting. Revisit before ~mid-2027.
- Components pinned in `firmware/s3/components/bsp/idf_component.yml`: `esp_lcd_sh8601` 1.0.0, `lvgl` 8.4.0; in
  `firmware/s3/main/idf_component.yml`: `esp_audio_codec` 2.6.2 (prebuilt MP3 decoder). `dependencies.lock` is committed.
  Its CMake adds an unquoted `-L` that breaks on the space in this repo's path; `firmware/s3/CMakeLists.txt` drops it.

## Layout
- `firmware/s3/main/`: app wiring and UI (thin); `player_task.c` = file -> MP3 decoder -> I2S (decisions live in logic)
- `firmware/s3/components/bsp/`: board drivers; pins only in `include/bsp/board_pins.h`
- `firmware/s3/components/logic/`: pure C, **no ESP-IDF headers** (knob, CST816, INDEX.BIN parser, seek maths, player state)
- `tests/host/`: tests for `logic` (incl. vendor knob equivalence; INDEX.BIN golden fixture shared with Python) and `sync_tunes.py`
- `tools/monitor.py`: serial capture; survives USB re-enumeration; `--print-port` gives the S3 port only
- `recordings/`: the user's `.m4a` memos (gitignored; file name = title). `recordings/not-synced/` is ignored by sync
- `markers/<title>.txt` (git-tracked): start time of each play. sync-tunes only creates missing files, after the user
  confirms (`--accept`); **never overwrite or edit one without asking.** Format: `markers/README.md`
- Tunes: MP3 CBR 32 kbps mono 32 kHz (user's listening test, 2026-11-15); tune ID = FNV-1a of the normalised title
- `docs/backups.md`: factory S3 flash backup checksum (file in `~/esp/backups/`, outside git)

## Rules
- **Never claim something works on hardware without log evidence or the user's confirmation.** You can't see the
  screen or hear audio: ask specific yes/no questions. Record results in the `docs/hardware.md` verification log.
- **Flash only the ESP32-S3.** Never flash, erase, or open the port of the ESP32-U4WDH without asking. Its port
  appears when the USB-C plug is flipped (WCH CH340 `1a86:7523`). Always take the port from `monitor.py --print-port`;
  never run `idf.py flash` without `-p`.
- **Keep drivers separate from logic.** Anything decidable without hardware goes in `components/logic` with host tests.
- **Never guess a pin.** Use only pins in `docs/hardware.md`; mark anything unconfirmed UNVERIFIED.
- **Commit after each working step** (`./test` passes; hardware steps verified as above).
- Don't use `idf.py monitor` (needs a TTY); use `./logs`.
- New user communication: user is new to ESP-IDF; explain choices briefly; stop after each agreed phase.

## Gotchas (details in docs/hardware.md)
- DAC mute pin XSMT is driven only by the ESP32 (IO32) and floats otherwise. If S3 audio is silent, suspect it first.
- Vendor audio demo uses left-justified I2S but the DAC is wired for standard I2S (FMT=GND). Our code uses Philips (plays
  correctly); whether the vendor's format was wrong is still UNVERIFIED. Volume 0-10 (3 dB steps) in NVS `player/volume`.
- DAC rates without MCLK: only 16/32/44.1/48 kHz (no 22.05/24 kHz). Verified: 32, 44.1, 48 kHz.
- Every S3 GPIO is already assigned; there are no spare pins.
- Opening USB-Serial-JTAG with the wrong DTR/RTS order resets the chip; `monitor.py` handles it.
