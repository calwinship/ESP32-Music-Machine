# ESP32 Music Machine

A pocket **practice player for Irish trad tunes**, running on the Waveshare **ESP32-S3-Knob-Touch-LCD-1.8**: a round
360×360 touch screen set in a rotary knob, with a headphone jack, a battery socket and 16 MB of flash.

Each tune is a voice-memo recording of the tune played two or three times, slow to fast. The player knows where each
play (each **speed**) starts, so you can jump straight to the speed you're practising, nudge back a couple of seconds
with the knob, and loop a section again and again. The tunes live on the board's micro-SD card (about 11 hours fit),
with a smaller copy in its own flash as a fallback, so no phone is needed.

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

You need the board plugged into the Mac, the right way up, and a Terminal open in this folder (or Claude Code: put
`!` in front of each command).

### Add new tunes

1. **Copy the recording in.** Put the Voice Memo in `recordings/` and name it after the tune, for example
   `recordings/Cup of Tea.m4a`. The file name (without `.m4a`) is the title shown on the board, fadas included. Add as
   many as you like at once. Recordings you're not ready for can wait in `recordings/not-synced/`, which is ignored.

2. **Check the plug.** Run `ls /dev/cu.usb*`. You want `/dev/cu.usbmodem…`; if you see `/dev/cu.usbserial…`, turn the
   USB-C plug over (see [Which chip am I plugged into?](#which-chip-am-i-plugged-into)).

3. **Sync.** Run:
   ```sh
   ./sync-tunes
   ```
   For each new recording it works out where every play starts and writes `markers/<title>.txt` for you (you never
   have to create that file). Then it converts every recording and writes them all to the SD card. The board shows
   "Syncing tunes" with a percentage; keep the cable in. It takes about 2½ minutes for 16 tunes, longer with more.
   At the end you should see:
   ```
   sync-tunes: written and read back in 137 s; the board now plays 17 tunes from the SD card
   ```
   followed by a list of the new tunes and the start times it guessed.

4. **Check the speeds on the board.** Open the tune list, pick the new tune, then tap each segment of the ring
   (teal = slow, amber = medium, coral = fast). Each tap should start that play from its beginning. The guess is
   right to within about 2 seconds most of the time, but not always, and occasionally it gets the number of plays
   wrong. Starting a second early is fine; starting late cuts off the first notes.

5. **Fix a start that's wrong** (only if step 4 found one):
   ```sh
   ./mark "Cup of Tea"
   ```
   For each play it plays the few seconds around the marked start and asks: press **Enter** if it's right, **t** to
   tap it again (the recording plays, and you press Enter the moment the play starts), **d** to delete a play that
   isn't there, **r** to replay. If the guess is badly off, `./mark "Cup of Tea" --all` plays the whole recording
   and you press Enter as each play starts (play 1 too), then **q**. It asks before saving. Then run `./sync-tunes`
   again to put the new times on the board.

6. **Save your work** (optional, recommended). The `markers/` files are the only thing you can't recreate from the
   recordings, so commit them to git: `git add markers && git commit -m "Markers for Cup of Tea"` (or ask Claude).

### Change or remove a tune

- **Rename:** rename the `.m4a` and its `markers/` file to the same new name, then `./sync-tunes`.
- **Remove:** move the `.m4a` into `recordings/not-synced/`, then `./sync-tunes`. Its markers file stays (it's
  ignored), so the times come back if you return the recording.
- **Icon and colour:** add `icon: leaf` (a name from `tools/icons.txt`, 54 to choose from) and/or `colour: #2EC4B6`
  lines to the tune's markers file, then `./sync-tunes`. Without them the tune gets a music note and a colour picked
  from its title. Format details: [`markers/README.md`](markers/README.md).

### If something goes wrong

| Message | What to do |
|---|---|
| `no ESP32-S3 port` | Plug in, or turn the USB-C plug over (step 2) |
| `the SD card does not hold tunes yet` | A new card, or the last sync was interrupted. `./sync-tunes --wipe-card` (erases the card) |
| `the board found no SD card` | Check the card is in. Meanwhile `./sync-tunes --built-in` fills the built-in storage |
| Cable pulled out during a sync | Nothing is lost: within a minute the board goes back to playing its built-in tunes. Run `./sync-tunes --wipe-card` |
| `markers/….txt:3: …` | That line of the markers file isn't a valid time; fix it with `./mark "<title>"` or by hand |

### Built-in storage (fallback)

The board also has 13.5 MB of its own flash for tunes, used when there's no SD card (or it isn't a tunes card). It
only changes when you run `./sync-tunes --built-in`, which writes 32 kbps copies and refuses if they don't fit
(about 16 tunes, 56 minutes).

---

## How it works

```
 your Mac                                             the board
 ─────────                                            ─────────
 recordings/*.m4a ─┐                                  micro-SD card (FAT16) ─┐ /tunes/T01.MP3 … INDEX.BIN
 markers/*.txt ────┤ ./sync-tunes                     flash (16 MB)          │ (the card if it has tunes,
                   ├─ guesses play starts (new tunes) ├ app (3 MB): firmware │  else the flash "tunes" area)
                   ├─ ffmpeg → MP3 96 kbps mono 32 kHz├ nvs: volume, resume  │
                   ├─ INDEX.BIN (titles, speeds,      └ tunes (13 MB): 32 kbps fallback (--built-in)
                   │  seek table, icon, colour)                 │
                   └─ FAT image → over USB to the card          ▼
                                                      player task: read 144-byte MP3 frames → decoder (esp_audio_codec)
                                                        → volume → I2S (32 kHz) → PCM5100A DAC → headphone jack
                                                      UI task (LVGL): screen, touch, knob → commands to the player
```

- **Format.** MP3, mono, 32 kHz, chosen by listening: 96 kbps on the SD card (a blind test ranked 96 > 64 > 48 > 32),
  32 kbps in the built-in flash, where space is tight. 32 kHz is one of the few rates the DAC can lock to without a
  master clock; this was tested on the board. Every MP3 frame of a tune is the same size, which makes seeking easy.
- **Writing the card.** `./sync-tunes` asks the running player to restart into sync mode, then sends the card image
  in checksummed 32 KB pieces; the board writes each one, reads it back and compares. The card's first sector is
  cleared first and written last, so a half-finished sync never looks like a valid card.
- **Finding the plays.** Play 1 starts at the first sustained sound. Later plays have no pause before them, so they're
  found by tempo: a rhythm fingerprint every half second, split where it changes, kept only where the next stretch is
  at least 15% faster. On the first 16 recordings it counted the plays right for 15 and put 20 of 28 starts within 2 s
  of the hand-set times; hence step 4.
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
./sync-tunes                 # recordings + markers -> the SD card   (--wipe-card, --built-in, --no-flash = don't touch the board)
./mark "<title>"             # check or re-tap where each play starts, by ear   (--all: tap them all from the beginning)
tools/gen_fonts.sh           # rebuild the fonts (only when adding icons or characters)
```

Run these in Terminal from this folder (or in Claude Code, prefixed with `!`).

### Which chip am I plugged into?

The board has two chips, and the USB-C socket has a separate connection to each. Which one your Mac sees depends on
which way round the plug goes in:

- **ESP32-S3**: runs this music player. This is the one you want.
- **ESP32-U4WDH**: runs Waveshare's factory software. Leave it alone.

To check, run:

```sh
ls /dev/cu.usb*
```

| You see | Meaning | Do |
|---|---|---|
| `/dev/cu.usbmodem…` | ESP32-S3 | Nothing, you're ready |
| `/dev/cu.usbserial…` | ESP32-U4WDH | Unplug, turn the plug over, plug back in |
| `no matches found` | Nothing connected | Push the plug in firmly; if still nothing, try another cable (some only charge) |

You don't have to check first: `./flash`, `./logs` and `./sync-tunes` only ever talk to the ESP32-S3, and if they say
no S3 port was found, flip the plug.

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
build flash logs test sync-tunes mark   one-word commands
markers/                           start times + icon/colour per tune (yours, tracked in git)
recordings/                        your .m4a memos (not in git)
firmware/s3/                       the ESP-IDF project
  main/                            the app: player task, UI, settings, fonts
  components/bsp/                  board drivers: the only code that touches hardware
  components/logic/                pure C rules, no ESP-IDF: unit-tested on the Mac
  partitions.csv                   flash layout (app 3 MB, tunes 13 MB)
tests/host/                        unit tests (C and Python), incl. an INDEX.BIN fixture shared by both
tools/                             sync_tunes.py, mark.py, board_link.py, gen_fonts.sh, icons.txt, monitor.py
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
| `sync_tunes.py` | Recordings → MP3 → `INDEX.BIN` → FAT image → SD card (or flash); guesses play starts for new tunes; never overwrites markers |
| `mark.py` | Plays the audio around each play start; keep, re-tap or delete; saves the markers file after asking |
| `board_link.py` | The Mac side of the USB link: framed, checksummed messages to the firmware; writes the card image |
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
- Play starts after play 1 are guessed by tempo and can be a few seconds off (check them on the board, fix with
  `./mark`).
- The card is written as one FAT16 volume, so cards up to 2 GB are used in full; a bigger card would need FAT32.
- Writing the card runs at about 0.3 MB/s (2½ minutes for 16 tunes; ~25 minutes if the card were nearly full).

The original board-bring-up tour of this repo (the `baseline` tag) is still there: `git show baseline:README.md`.

## Credits

- Board, schematic, wiki text (`docs/ref/`) and the panel start-up sequence: Waveshare's published resources for
  this board.
- Knob decoding is a port of Espressif's `bidi_switch_knob.c` (Apache-2.0); MP3 decoding is Espressif's
  `esp_audio_codec` 2.6.2.
- Fonts: Montserrat and Font Awesome 5 Free (as shipped with LVGL), converted with `lv_font_conv`.
- Built with ESP-IDF v5.5.6 and LVGL 8.4.0.
