# ESP32 Music Machine

A pocket **practice player for Irish trad tunes**, running on the Waveshare **ESP32-S3-Knob-Touch-LCD-1.8**: a round
360×360 touch screen set in a rotary knob, with a headphone jack, a battery socket and 16 MB of flash.

Each tune is a voice-memo recording of the tune played two or three times, slow to fast. The player knows where each
play (each **speed**) starts, so you can jump straight to the speed you're practising, nudge back a couple of seconds
with the knob, and loop a section again and again. All tunes live in the board's own flash, so no SD card or phone
is needed.

```
          ▭                   battery (only when not on USB)
          ❚❚                  shown while paused
          🍃                  the tune's icon, in the tune's colour (the only colour on a black screen)
       Clare Jig              wraps to at most two lines inside the ring
         1:42                 elapsed time
         Medium               the current speed, in its colour
          ≡                   tune list
   ring round the edge: the whole tune, one segment per speed (teal, amber, coral),
   each as long as that speed lasts; the played part bright, the rest dim
```

---

## What it does

| Feature | How |
|---|---|
| **Play / pause** | Tap the screen |
| **Jump back or forward** | Turn the knob: 2 s per click; fast turns add up |
| **Choose a speed** | Tap that speed's segment of the ring round the edge; it plays from its start |
| **Start this speed again** | Double-tap |
| **Volume** | Press and hold, then turn the knob (0–10, saved) |
| **Pick a tune** | Tap ≡ at the bottom; scroll (one tune per flick) or turn the knob; tap a tune to play it; swipe right to go back |
| **How to use** | The last page of the tune list; also shown by itself on the very first start |
| **Picks up where you left off** | After power-off, the same tune at the same place, paused |
| **Battery** | Small icon at the top while on battery; hidden on USB |
| **Themes** | Every tune has its own icon and colour (set in its `markers/` file) |
| **Irish spelling** | Titles keep their fadas: *Nóra Chríonna* |

The board's other chip (an ESP32 running Waveshare's factory firmware) is left alone.

---

## Adding or changing tunes

1. Put the recording in `recordings/` as `<Tune title>.m4a` (any voice memo works). The file name becomes the title.
2. Run `./sync-tunes --no-flash`. For a new tune it **proposes** when play 1 starts and prints it, but writes nothing.
3. Accept that with `./sync-tunes --accept "<Tune title>"`. That writes `markers/<Tune title>.txt`. Then, by hand, add
   the start times of plays 2 and 3 (find them in QuickTime):
   ```
   # Clare Jig
   0:06.8
   1:41.0
   2:37.8
   icon: leaf
   colour: #2EC4B6
   ```
   `icon:` is a name from `tools/icons.txt` (54 to choose from) and `colour:` is any `#RRGGBB`; both are optional.
   Full format: [`markers/README.md`](markers/README.md).
4. Run `./sync-tunes` with the board plugged in. It converts every recording, builds the tune image, and writes
   **only** the tunes area of the flash. That takes about 2–3 minutes, and your volume and resume point are kept.

`sync-tunes` never overwrites a markers file, and it refuses, with the numbers, if the tunes don't fit. The current
16 tunes are 56 minutes, which fills the 13.5 MB tunes area; an SD card or online sync is the planned way to grow.

---

## How it works

```
 your Mac                                             the board
 ─────────                                            ─────────
 recordings/*.m4a ─┐                                  flash (16 MB)
 markers/*.txt ────┤ ./sync-tunes                     ├ app (3 MB): this firmware
                   ├─ ffmpeg → MP3 32 kbps mono 32 kHz├ nvs: volume, resume point, "guide seen"
                   ├─ INDEX.BIN (titles, speeds,      └ tunes (13 MB, read-only FAT) ── /tunes/T01.MP3 … INDEX.BIN
                   │  seek table, icon, colour)                 │
                   └─ FAT image → writes only "tunes"           ▼
                                                      player task: read 144-byte MP3 frames → decoder (esp_audio_codec)
                                                        → volume → I2S (32 kHz) → PCM5100A DAC → headphone jack
                                                      UI task (LVGL): screen, touch, knob → commands to the player
```

- **Format.** MP3 at 32 kbps, mono, 32 kHz: you chose it by listening. 32 kHz is one of the few rates the DAC can lock
  to without a master clock; this was tested on the board. Each MP3 frame is a fixed 144 bytes, which makes seeking easy.
- **Exact seeking.** `INDEX.BIN` stores, for every second of every tune, where decoding has to start. MP3 frames lean
  on earlier frames (the "bit reservoir"), so that point is a little before the second itself. The player decodes from
  there and throws away the extra, so a jump lands on the exact sample. This was checked against a full decode on the
  Mac: 75 of 75 positions identical.
- **Speeds** are the stretches between the start times in a markers file. The ring round the edge is the whole tune,
  split into one segment per speed in proportion to its length, and fills in as the tune plays.
- **Audio never waits for the screen.** The player runs as its own task on the second CPU core, with ~120 ms of
  buffered audio. It counts every buffer underrun and logs it; a full tune plays with `underruns 0`.
- **Jumps don't click:** the sound fades out and back in over 5 ms around every seek or speed change.
- **Saving is gentle on the flash:** the resume point is saved every 30 s while playing (and 2 s after you stop), and
  the volume is saved 2 s after the last change.
- **Boot:** the backlight stays off until the first screen is drawn, then fades in, so there's no white flash. It's
  ready in about 1.5 s.
- **Battery:** the sense pin reads the battery itself when unplugged, but the USB supply when plugged in. So the
  icon is shown only on battery (smoothed over ~30 s) and hidden on USB.
- **Titles** are measured in the real font at boot: every one must fit in two lines well inside the ring. The log
  shows the result (`titles: 16 checked, 0 over 2 lines; farthest corner 134 px`).

---

## Commands

```sh
./build                      # compile the firmware
./flash                      # compile + flash the ESP32-S3 + show 8 s of boot logs
./logs 60                    # reset the board and print 60 s of logs   (--reset none = don't reset)
./test                       # unit tests on the Mac (C logic with sanitizers + sync-tunes in Python); no board
./sync-tunes                 # recordings + markers -> the board's tunes area   (--no-flash, --accept "<title>", --accept-all)
tools/gen_fonts.sh           # rebuild the fonts (only when adding icons or characters)
```

The USB-C plug works both ways round, but each way reaches a different chip. `./flash` only ever flashes the ESP32-S3;
if it says no S3 port was found, flip the plug.

First-time setup on a Mac:

```sh
brew install python@3.12 cmake ninja dfu-util ffmpeg
mkdir -p ~/esp/v5.5.6 && cd ~/esp/v5.5.6
git clone -b v5.5.6 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH" ./install.sh esp32s3
python3 tools/idf_tools.py install cmake        # ESP-IDF's tested CMake (3.30.2)
```

---

## Code map

```
build flash logs test sync-tunes   one-word commands
markers/                           start times + icon/colour per tune (yours, tracked in git)
recordings/                        your .m4a memos (not in git)
firmware/s3/                       the ESP-IDF project
  main/                            the app: player task, UI, settings, fonts
  components/bsp/                  board drivers: the only code that touches hardware
  components/logic/                pure C rules, no ESP-IDF: unit-tested on the Mac
  partitions.csv                   flash layout (app 3 MB, tunes 13 MB)
tests/host/                        unit tests (C and Python), incl. an INDEX.BIN fixture shared by both
tools/                             sync_tunes.py, gen_fonts.sh, icons.txt, monitor.py (log capture)
docs/hardware.md                   every pin, which chip owns what, and the hardware verification log
```

The split is deliberate. If a decision can be made without hardware, it lives in `components/logic` and has a test.
Drivers stay thin, and `main/` wires the two together.

### The functions, briefly

**App (`firmware/s3/main/`)**

| | |
|---|---|
| `app_main` | Loads settings, starts the screen, mounts the tunes, starts the player and the UI |
| `player_task_start` | Opens `INDEX.BIN`, then resumes the saved tune (paused) or plays the first one |
| `player_task_tap / _seek / _play_speed / _restart_speed / _select_tune` | Commands to the player; safe from any task |
| `player_status_get` | A consistent snapshot for the screen: tune, time, speed, progress, state |
| `player_tune_brief` | Title, colour, icon, length and number of speeds of any tune (for the list) |
| `ui_start` | Builds the tune screen, timeline ring, tune list and guide, and routes touch and knob |
| `settings_*` | Volume, resume point and "guide seen", kept in NVS (flash that survives power-off and re-syncs) |

**Board drivers (`components/bsp/`)**

| | |
|---|---|
| `bsp_display_start`, `bsp_display_lock/unlock` | Screen, touch and LVGL; the lock makes LVGL calls thread-safe |
| `bsp_backlight_set`, `bsp_backlight_fade_in` | Screen brightness; the boot fade |
| `bsp_knob_start`, `bsp_knob_take_delta` | Knob clicks since you last asked (+ clockwise) |
| `bsp_audio_init/start`, `bsp_audio_write_mono/_silence` | I2S output to the DAC, with volume |
| `bsp_audio_underruns` | How many times the audio ran dry (should stay 0) |
| `bsp_storage_mount` | Mounts the read-only tunes area at `/tunes` |
| `bsp_supply_mv` | Battery or USB voltage |

**Rules (`components/logic/`, host-tested)**

| | |
|---|---|
| `tune_index_open/get`, `tune_second` | Reads and fully validates `INDEX.BIN`; per-second seek points |
| `seek_plan`, `frame_gate_*` | Where to start decoding for an exact jump, and which decoded samples to drop |
| `player_*` | Player state machine: playing/paused/ended, position, underrun accounting |
| `speed_at`, `speed_start`, `speed_permille` | Which speed a position is in, where it starts, how far through |
| `resume_find` | Turns a saved tune ID and time back into a tune and sample (survives re-syncs and reordering) |
| `knob_seek_*`, `seek_target` | Combines quick knob clicks into one jump; clamps to the tune |
| `tap_detector_*` | Tells a single tap from a double-tap |
| `volume_*` | Volume steps (3 dB each) and the 3 s volume mode |
| `resume_should_save` | Decides when the resume point is worth a flash write |
| `fade_ramp_to_zero`, `fade_in` | The click-free fades around jumps |
| `battery_update` | Voltage → battery level, smoothing and "plugged in" |
| `speed_rgb`, `timeline_*`, `angle_d10`, `ring_hit` | Speed colours; the timeline ring's segments, played part and which segment a tap hits |
| `knob_decoder_*`, `cst816_parse` | Raw knob pins → clicks; touch chip report → x/y |
| `text_mmss`, `text_utf8`, `text_fold_ascii` | Time and text formatting for the screen |

**Tools (`tools/`)**

| | |
|---|---|
| `sync_tunes.py` | Recordings → MP3 → `INDEX.BIN` → FAT image → the board; proposes play-1 start times; never overwrites markers |
| `gen_fonts.sh` | Builds the fonts: text with fadas, large digits, and the tune icons listed in `icons.txt` |
| `monitor.py` | Captures logs and survives the USB port vanishing when the board resets |

---

## Status and caveats

Everything above has been checked on the board, with logs and by ear and eye; the record is in
[`docs/hardware.md`](docs/hardware.md). Still open:

- The battery level curve is a generic lithium one, not yet measured over a full discharge on this board, so the icon
  is coarse on purpose.
- Whether Waveshare's own audio demo used the wrong I2S format is UNVERIFIED. This player uses the format the DAC is
  wired for, and plays correctly.
- The DAC's mute pin is driven by the *other* chip's factory firmware. If audio is ever silent, suspect that first.
- Detecting the start of plays 2 and 3 automatically was set aside; they're typed in by hand.

The original board-bring-up tour of this repo (the `baseline` tag) is still there: `git show baseline:README.md`.

## Credits

- Board, schematic, wiki text (`docs/ref/`) and the panel start-up sequence: Waveshare's published resources for
  this board.
- Knob decoding is a port of Espressif's `bidi_switch_knob.c` (Apache-2.0); MP3 decoding is Espressif's
  `esp_audio_codec` 2.6.2.
- Fonts: Montserrat and Font Awesome 5 Free (as shipped with LVGL), converted with `lv_font_conv`.
- Built with ESP-IDF v5.5.6 and LVGL 8.4.0.
