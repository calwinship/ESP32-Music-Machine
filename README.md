# ESP32 Music Machine

Firmware foundation for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8**: a round 360×360 touch screen inside a rotary knob,
with a microphone, a DAC with a headphone jack, haptics and a TF card. Its two chips are:

- **ESP32-S3**, which runs everything in this repo;
- **ESP32-U4WDH**, which runs Waveshare's factory firmware and is left alone.

The `baseline` tag is a clean, hardware-verified starting point:
- the board drivers work;
- a tiny demo app shows a number on an arc that the knob turns up and down;
- host unit tests and one-word build commands are in place.

New projects start from that tag (see [Starting a new project from the baseline](#starting-a-new-project-from-the-baseline)).

This README is a guided tour for someone who knows software but not embedded C.
The hardware facts (every pin, which chip owns what, and what is still unverified) live in **[docs/hardware.md](docs/hardware.md)**.

---

## Quick start

```sh
./build          # compile the firmware
./flash          # compile + flash the ESP32-S3 + show 8 s of boot logs
./logs 20        # reset the board and print 20 s of logs   (./logs 20 --reset none = don't reset)
./test           # run the unit tests on your Mac (no board needed)
```

The board's USB-C plug is **reversible but not symmetric**: one way round you reach the ESP32-S3 (USB ID `303A:1001`);
flipped, you reach the other chip's USB-serial bridge (`1a86:7523`). `./flash` refuses to flash the wrong chip.
If it says no S3 port was found, flip the plug.

First-time setup on a new Mac (what this repo was built with):

```sh
brew install python@3.12 cmake ninja dfu-util
mkdir -p ~/esp/v5.5.6 && cd ~/esp/v5.5.6
git clone -b v5.5.6 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH" ./install.sh esp32s3
python3 tools/idf_tools.py install cmake        # ESP-IDF's tested CMake (3.30.2)
```

`tools/idf_env.sh` puts Python 3.12 first and loads that ESP-IDF. `./build`, `./flash` and `./logs` source it for you; `./test` doesn't need it.

---

## A five-minute primer: what's different from "normal" software

| You know… | Here it's… |
|---|---|
| An OS that loads your program | No OS in the usual sense. The chip boots straight into one program (the "app"), which links in a small real-time kernel, **FreeRTOS**. |
| Threads | FreeRTOS **tasks**. There are also **esp_timer callbacks**: small functions the system calls every N microseconds. |
| Libraries / packages | ESP-IDF **components**: a folder with a `CMakeLists.txt` that calls `idf_component_register(...)`. Third-party ones come from Espressif's registry and are declared in `idf_component.yml`, then downloaded into `managed_components/`. |
| Config files / env vars | **Kconfig**: compile-time switches (`CONFIG_...`). Ours are in `firmware/s3/sdkconfig.defaults`; the build expands them into a full `sdkconfig`. |
| A disk | **Flash memory**, split by `partitions.csv`: bootloader at 0x0, partition table at 0x8000, our app at 0x10000. |
| `print` debugging | `ESP_LOGI(TAG, ...)`. The text goes out over USB, and `./logs` captures it. |
| `if err != nil` | Functions return `esp_err_t`. `ESP_ERROR_CHECK(x)` aborts and reboots on failure; `ESP_RETURN_ON_ERROR(x, ...)` returns the error to the caller. |
| A UI framework | **LVGL v8**, a C GUI library. It is **not thread-safe**, so every LVGL call happens while holding one mutex. |

Two habits matter more than in desktop code:
- **Don't block in callbacks.** Timer callbacks run in a shared system task.
- **Don't touch hardware outside `components/bsp`.** That rule is what makes the rest testable on a laptop.

---

## Repository layout

```
build  flash  logs  test      one-word commands (zsh scripts)
CLAUDE.md                     working rules + commands (also read by Claude Code)
firmware/s3/                  the ESP-IDF project for the ESP32-S3
  main/main.c                 the app: wiring + UI
  components/bsp/             board drivers: the ONLY code that touches hardware
  components/logic/           pure C, no ESP-IDF headers, unit-tested on the Mac
  sdkconfig.defaults          Kconfig settings (PSRAM, flash size, LVGL options…)
  partitions.csv              flash layout
  dependencies.lock           exact versions of downloaded components (committed)
tests/host/test_logic.c       unit tests for components/logic
tools/monitor.py              log capture that survives the USB port vanishing on reset
tools/idf_env.sh              loads the pinned ESP-IDF
tools/fetch_vendor.sh         re-downloads Waveshare's demos/schematic/datasheets into docs/vendor/ (gitignored)
docs/hardware.md              pins, chips, inter-chip links, verification log, UNVERIFIED list
docs/backups.md               checksum of the factory flash backup (the backup itself lives outside git)
docs/ref/                     schematic pages, a text copy of the Waveshare wiki, vendor-file checksums
```

---

## The three parts of the firmware

### `components/bsp/`: board support ("drivers")

The only code that knows this is an ESP32-S3 on a particular circuit board. Its public interface is
[`include/bsp/bsp.h`](firmware/s3/components/bsp/include/bsp/bsp.h), six functions:

| Function | What it does |
|---|---|
| `bsp_display_start()` | Turns on the backlight, sets up the screen and touch controller, starts LVGL and its task |
| `bsp_display_lock()` / `bsp_display_unlock()` | The LVGL mutex. Take it before calling LVGL from your own task |
| `bsp_backlight_set(duty)` | Screen brightness, 0–255 |
| `bsp_knob_start()` | Starts sampling the knob every 3 ms |
| `bsp_knob_take_delta()` | "How many clicks since I last asked?" (+ clockwise, − anticlockwise) |

Inside it:
- [`board_pins.h`](firmware/s3/components/bsp/include/bsp/board_pins.h) is the single source of truth for GPIO numbers; each one is cited in `docs/hardware.md`.
- [`bsp_display.c`](firmware/s3/components/bsp/bsp_display.c) holds the screen, touch and LVGL glue.
- [`bsp_knob.c`](firmware/s3/components/bsp/bsp_knob.c) samples the knob.
- [`lcd_init_cmds.h`](firmware/s3/components/bsp/lcd_init_cmds.h) holds the panel's 185-command startup sequence, copied by script from Waveshare's working demo.

The display driver (`esp_lcd_sh8601` 1.0.0) and LVGL (8.4.0) are pinned in `bsp/idf_component.yml`.

### `components/logic/`: hardware-independent rules

Plain C11 that includes nothing from ESP-IDF, so `./test` compiles it with the Mac's own compiler.
- [`knob.c`](firmware/s3/components/logic/knob.c) turns raw pin samples into clicks (debouncing) and keeps a value clamped to a range.
- [`cst816.c`](firmware/s3/components/logic/cst816.c) decodes the touch chip's 7-byte report into x/y.

Rule of thumb: if a decision can be made without hardware, it belongs here and gets a test.
The tests include one that runs Waveshare/Espressif's original knob code side by side with ours on 200,000 random
inputs and requires identical results.

### `main/`: the application

[`main.c`](firmware/s3/main/main.c) is deliberately thin:
- it starts the BSP;
- it builds the UI (arc + number + hint);
- it runs one LVGL timer that turns knob clicks into value changes;
- a screen tap resets the value to 50.

Every change is logged (`app: value=51 (knob cw)`), so behaviour can be checked from logs as well as by eye.
When you build the tune learner, this is where the app goes. New hardware drivers go in `bsp`, and new rules go in `logic`.

---

## Trace: one clockwise click, from your fingers to the arc

**1. The physical switch.** The knob isn't a quadrature encoder. Each detent closes one of two contacts: **A for
clockwise, B for anticlockwise**. Contact A connects the `EC1_A` line to ground. That line is normally held high by a
10 kΩ pull-up resistor, so the S3 sees **GPIO 8** go from 1 to 0 for a few milliseconds, then back to 1.
(Schematic pages 1–2; `BSP_KNOB_PIN_A` in `board_pins.h`.)

**2. Sampling, every 3 ms.** At boot, `bsp_knob_start()` (`bsp/bsp_knob.c`) configured GPIO 7 and 8 as inputs and
started a periodic **esp_timer**. Every 3 ms the system calls `knob_sample_cb()` (same file). It reads both pins with
`gpio_get_level()` and hands the two levels to the logic layer.

**3. Debouncing, in pure C.** `knob_decoder_step()` (`logic/knob.c`) runs a tiny state machine for each line in
`channel_step()`. It counts how many samples the line has been low. When the line goes high again after **at least 2
low samples** (≥ 6 ms), that was a real click, and it returns **+1**. A single-sample blip is contact bounce and is ignored.
This is why the click registers when the detent *releases*, not when it first closes.

**4. Handing the click across threads.** Back in `knob_sample_cb()`, the +1 is added to an atomic counter
(`atomic_fetch_add(&s_delta, …)`). The timer callback runs in the system's timer task, and the UI runs in the LVGL
task, so a lock-free atomic is the simplest safe handover.

**5. The UI picks it up, within 20 ms.**
- `bsp_display_start()` created a FreeRTOS task, `lvgl_task()` (`bsp/bsp_display.c`). It loops forever: take the LVGL
  mutex, call `lv_timer_handler()`, release, sleep.
- `lv_timer_handler()` runs LVGL's timers, including the one `main.c` registered in `ui_create()`:
  `knob_poll_timer()`, every 20 ms.
- `knob_poll_timer()` calls `bsp_knob_take_delta()`, which atomically reads the counter and resets it to zero.

**6. Applying the rule.** `knob_value_apply()` (`logic/knob.c`) adds the delta and clamps to 0–100. It returns
`true` only if the value actually changed, so at 100 more clockwise clicks do nothing.

**7. Updating the widgets.** `show_value()` (`main.c`) calls `lv_arc_set_value()` and `lv_label_set_text_fmt()`, and
logs `app: value=51 (knob cw)`. These calls don't draw anything yet. They mark the arc and the label as "dirty".

**8. Rendering.**
- On LVGL's next screen refresh (every 20 ms, `CONFIG_LV_DISP_DEF_REFR_PERIOD`), still inside `lv_timer_handler()`,
  LVGL redraws only the dirty areas into a small RAM buffer (360 × 36 pixels).
- Our `lvgl_rounder_cb()` first widens each area to even pixel coordinates, which this panel requires.
- LVGL then calls `lvgl_flush_cb()`. That calls `esp_lcd_panel_draw_bitmap()`, and the `esp_lcd_sh8601` driver sends
  the pixels to the screen over **QSPI** (SPI2: clock on GPIO 13, CS on GPIO 14, 4 data lines on GPIO 15–18) using DMA.
- When the transfer finishes, the driver calls our `flush_ready_cb()`, which tells LVGL (`lv_disp_flush_ready()`) it
  may reuse the buffer for the next chunk.

**9. Photons.** The panel latches the pixels and the arc grows by one step.
Worst-case latency is roughly the click's release + up to 20 ms poll + up to 20 ms refresh + transfer time.

```
finger → contact A → GPIO8 ─(every 3 ms)→ knob_sample_cb → knob_decoder_step → atomic s_delta
     LVGL task: lv_timer_handler → knob_poll_timer → bsp_knob_take_delta → knob_value_apply → show_value
     → (dirty) → refresh → lvgl_rounder_cb → lvgl_flush_cb → esp_lcd_panel_draw_bitmap → QSPI/DMA → screen
```

A tap takes a similar path. LVGL polls `lvgl_touch_cb()` (`bsp_display.c`), which reads 7 bytes from the CST816 touch
chip over I2C (GPIO 11/12) and decodes them with `cst816_parse()` (`logic/cst816.c`). The tap becomes an
`LV_EVENT_CLICKED` on the screen, which calls `screen_clicked()` in `main.c`.

---

## What happens at boot

You can watch all of this with `./logs`:

1. **ROM bootloader** (burned into the chip). It prints `ESP-ROM:esp32s3-…` and loads the second-stage bootloader from flash address 0x0.
2. **Second-stage bootloader** (`boot: ESP-IDF v5.5.6 2nd stage bootloader`). It reads the partition table at 0x8000 and loads our app from the `factory` partition at 0x10000.
3. **ESP-IDF startup**, before any of our code runs:
   - it finds and tests the 8 MB PSRAM (`esp_psram: Found 8MB PSRAM device`);
   - it copies code and constants into it, sets up the heaps, starts FreeRTOS on both cores;
   - then it calls our `app_main()` from the `main_task`.
4. **`app_main()`** (`main/main.c`):
   1. `knob_value_init()`: value = 50, range 0–100.
   2. `bsp_display_start()`, in order:
      - backlight PWM on GPIO 47 at full;
      - SPI bus for the panel, then the panel driver;
      - a hardware reset (GPIO 21) and the 185 init commands (log: `sh8601: LCD panel create success, version: 1.0.0`);
      - the I2C bus, plus a write that puts the touch chip in normal mode;
      - `lv_init()` and two DMA draw buffers;
      - registration of the display and touch with LVGL;
      - a 2 ms tick timer, the mutex, and the LVGL task (`bsp_display: display started (360x360)`).
   3. `bsp_knob_start()`: knob pins plus the 3 ms sampling timer (`bsp_knob: knob sampling every 3 ms`).
   4. Under the LVGL lock: `ui_create()` builds the widgets, then `show_value("start")` (`app: value=50 (start)`).
   5. Logs `app: ready` and **returns**.
5. **After `app_main()` returns**, nothing stops. The LVGL task, the tick timer and the knob timer keep running forever.
   Returning from `app_main` just ends the startup task.

Any `ESP_ERROR_CHECK` failure during step 4 prints the error and reboots, so a boot loop in `./logs` means an init step failed.

---

## Starting a new project from the baseline

`baseline` is an annotated tag on the commit that matches this README. To start a second knob project from it:

```sh
git clone --branch baseline https://github.com/calwinship/ESP32-Music-Machine.git my-new-knob-project
cd my-new-knob-project
git switch -c main                       # leave the detached tag state
git remote set-url origin https://github.com/calwinship/<new-repo>.git
tools/fetch_vendor.sh                    # re-create docs/vendor/ (gitignored)
./test && ./flash
```

To get back to the clean state inside this repo: `git switch --detach baseline`, or diff against it with `git diff baseline`.

---

## Status and caveats

Verified on hardware (logs plus user confirmation; full log in `docs/hardware.md`): display, touch, knob, the starter app,
and Waveshare's audio demo (mic loopback and music playback). Still UNVERIFIED and worth knowing before audio work:

- **The DAC's mute pin is controlled only by the *other* chip** (ESP32 IO32) and floats if that chip doesn't drive it.
- **Possible audio format mismatch:** Waveshare's audio demo sends left-justified I2S, but the DAC is wired for standard I2S.
- **The serial link between the two chips** (S3 GPIO 38/48 ↔ ESP32 IO18/23): direction, speed and protocol are unknown.

The factory S3 firmware was backed up before the first flash; see `docs/backups.md` for the checksum and restore command.

## Credits

- Board, schematic, wiki text (`docs/ref/`) and the panel init sequence (`lcd_init_cmds.h`): from Waveshare's published
  resources for this board, used for reference.
- The knob decoding in `logic/knob.c` is a port of Espressif's `bidi_switch_knob.c` (Apache-2.0), shipped in Waveshare's demos.
- Built with ESP-IDF v5.5.6, LVGL 8.4.0 and `esp_lcd_sh8601` 1.0.0.
