#!/usr/bin/env python3
"""Convert recordings/ to device tunes, write INDEX.BIN, build a FAT image and write it to the board's SD card
(default) or, with --built-in, to ONLY the S3's `tunes` flash partition.

Run through ./sync-tunes (IDF Python env). Steps:
  1. recordings/*.m4a -> title (file name, trimmed, NFC) and a stable tune ID (FNV-1a of the normalised title).
  2. ffmpeg -> MP3 CBR mono (cached in build-tunes/cache). Our own parser walks the frames, drops the Info/LAME frame
     and records, for every second, where decoding must start so that seeking to any sample in it is exact.
  3. markers/<title>.txt (git-tracked) gives the start time of each play. Missing file -> sync-tunes guesses every
     play's start (play 1 by loudness, later plays by tempo) and writes the file; it never changes an existing file.
     ./mark "<title>" (tools/mark.py) checks or re-taps the starts by ear.
  4. Size check: SD card -> a FAT16 volume just big enough for the files (checked against the card the board reports);
     --built-in -> the `tunes` partition in firmware/s3/partitions.csv. Refuse if it doesn't fit.
  5. IDF fatfsgen.py -> raw FAT image.
     SD card: tools/board_link.py asks the running firmware to restart into sync mode, sends the image in checksummed
     pieces (the board writes, reads back and compares each), with sector 0 cleared first and written last so an
     interrupted sync never leaves a half-valid card; then the board restarts as the player and must report N tunes
     from the SD card. A card that does not hold tunes yet is only erased with --wipe-card.
     --built-in: parttool.py writes the `tunes` partition (port from tools/monitor.py --print-port, ESP32-S3 only).

Only the Python standard library is used here, so tests/host/test_sync_tunes.py runs with any python3.
"""
import argparse
import colorsys
import datetime
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import unicodedata
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECORDINGS = ROOT / 'recordings'
MARKERS = ROOT / 'markers'
BUILD = ROOT / 'build-tunes'
PARTITIONS_CSV = ROOT / 'firmware' / 's3' / 'partitions.csv'
PARTITION_NAME = 'tunes'
ICONS_FILE = ROOT / 'tools' / 'icons.txt'
DEFAULT_ICON = 'music'
RECORDING_EXTS = ('.m4a',)

# Tune format, chosen 2026-11-15 by listening test: MP3 CBR, mono, 32 kHz (a DAC rate verified on the board).
# The bitrate is the only knob (same decoder, same firmware): SD card 96 kbps (blind test 2026-11-25 ranked
# 96 > 64 > 48 > 32, "all pretty much the same"); built-in flash 32 kbps, which is what fits the partition.
SD_BITRATE_KBPS = 96
FLASH_BITRATE_KBPS = 32
SAMPLE_RATE = 32000
CHANNELS = 1

MP3_DECODER_DELAY = 529     # samples every MP3 decoder adds on top of the LAME tag's encoder delay (ffmpeg, LAME)
MAX_MARKERS = 8
MAX_TITLE_BYTES = 100
SECTOR = 4096               # FAT sector = flash sector, as IDF's fatfs_create_rawflash_image uses
SD_SECTOR = 512             # FAT sector on the SD card = the card's sector (FatFs needs them equal)
ROOT_ENTRIES = 512          # fatfsgen default
FAT12_MAX_CLUSTERS = 4085   # FatFs: <= this many clusters is FAT12

INDEX_MAGIC = b'TIDX'
INDEX_VERSION = 2
CODEC_MP3 = 1
INDEX_HEADER = struct.Struct('<4sHHB3xI')            # magic, version, tune count, codec, total length (incl. CRC)
INDEX_RECORD = struct.Struct('<IIIIIHHHBBBBHI')     # see write_index()
INDEX_SECOND = struct.Struct('<II')                 # byte offset, frame number where decoding starts


class SyncError(Exception):
    """A problem the user must fix; printed without a traceback."""


# ---------------------------------------------------------------- titles and stable IDs

def tune_title(file_stem):
    """Display title: the recording's file name, NFC (macOS hands out NFD), outer whitespace trimmed."""
    return unicodedata.normalize('NFC', file_stem).strip()


def tune_key(title):
    """Matching key: NFC, case-folded, runs of whitespace collapsed. 'Ratlan bog ' and 'ratlan  Bog' match."""
    folded = unicodedata.normalize('NFC', unicodedata.normalize('NFC', title).casefold())
    return ' '.join(folded.split())


def fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def tune_id(title):
    """Stable 32-bit ID from the normalised title, for play history in NVS. Renaming a tune changes its ID."""
    return fnv1a32(tune_key(title).encode('utf-8'))


# ---------------------------------------------------------------- marker files

_TIME_RE = re.compile(r'^(?:(\d+):)?(\d+(?:\.\d+)?)$')


def parse_time_ms(text):
    """'1:25.4' or '85.4' -> 85400. Seconds must be < 60 when minutes are given."""
    m = _TIME_RE.match(text)
    if not m:
        raise ValueError(f'not a time: {text!r} (use m:ss.s, e.g. 1:25.4)')
    minutes = int(m.group(1)) if m.group(1) is not None else 0
    seconds = float(m.group(2))
    if m.group(1) is not None and seconds >= 60:
        raise ValueError(f'seconds must be under 60 in {text!r}')
    return int(round((minutes * 60 + seconds) * 1000))


def format_time_ms(ms):
    tenths = int(round(ms / 100))
    return f'{tenths // 600}:{(tenths % 600) / 10:04.1f}'


def load_icons(path=ICONS_FILE):
    """tools/icons.txt -> {name: FontAwesome codepoint}. The same list builds the device's icon font."""
    icons = {}
    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.split('#', 1)[0].split()
        if line:
            icons[line[0].lower()] = int(line[1], 16)
    return icons


def default_colour(tid):
    """A tune's colour when its file has none: a bright hue picked from its stable ID (so it never changes)."""
    r, g, b = colorsys.hsv_to_rgb((tid % 360) / 360.0, 0.6, 0.95)
    return (round(r * 255) << 16) | (round(g * 255) << 8) | round(b * 255)


@dataclass
class TuneFile:
    starts: list
    icon: str = None      # name from tools/icons.txt
    colour: int = None    # 0xRRGGBB


_KEY_RE = re.compile(r'^([A-Za-z]+)\s*:\s*(.*)$')
_COLOUR_RE = re.compile(r'^#?([0-9A-Fa-f]{6})$')


def parse_tune_file(text, duration_ms, where, icons=None):
    """Strictly parse markers/<title>.txt: one start time per line, plus optional `icon: <name>` and
    `colour: #RRGGBB` lines (`color` also accepted); '#' followed by a space or at line start is a comment.
    icons = load_icons() to check icon names (None skips the check)."""
    out = TuneFile(starts=[])
    starts = out.starts
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = re.split(r'(?:^|\s)#(?![0-9A-Fa-f]{6}\b)', raw, maxsplit=1)[0].strip()
        if not line:
            continue
        key = _KEY_RE.match(line)
        if key:
            name, value = key.group(1).lower(), key.group(2).strip()
            if name == 'icon':
                if out.icon is not None:
                    raise SyncError(f'{where}:{lineno}: icon given twice')
                value = value.lower()
                if icons is not None and value not in icons:
                    raise SyncError(f'{where}:{lineno}: unknown icon {value!r} (see tools/icons.txt)')
                out.icon = value
            elif name in ('colour', 'color'):
                if out.colour is not None:
                    raise SyncError(f'{where}:{lineno}: colour given twice')
                m = _COLOUR_RE.match(value)
                if not m:
                    raise SyncError(f'{where}:{lineno}: colour must look like #2EC4B6')
                out.colour = int(m.group(1), 16)
            else:
                raise SyncError(f'{where}:{lineno}: unknown setting {name!r} (icon, colour)')
            continue
        try:
            ms = parse_time_ms(line)
        except ValueError as e:
            raise SyncError(f'{where}:{lineno}: {e}') from None
        if starts and ms <= starts[-1]:
            raise SyncError(f'{where}:{lineno}: {line} is not after the previous start ({format_time_ms(starts[-1])})')
        if ms >= duration_ms:
            raise SyncError(f'{where}:{lineno}: {line} is past the end of the tune ({format_time_ms(duration_ms)})')
        starts.append(ms)
    if len(starts) > MAX_MARKERS:
        raise SyncError(f'{where}: {len(starts)} start times; at most {MAX_MARKERS}')
    return out


def parse_markers(text, duration_ms, where):
    """Start times only (see parse_tune_file)."""
    return parse_tune_file(text, duration_ms, where).starts


def marker_file_text(title, starts_ms, today, how='guessed by sync-tunes', icon=None, colour=None):
    lines = [f'# Start time of each play of "{title}" (m:ss.s), one per line, slow to fast.',
             f'# {how[0].upper() + how[1:]} on {today}. Check each speed on the board; fix with ./mark "{title}".']
    lines += [format_time_ms(ms) for ms in starts_ms]
    if icon:
        lines.append(f'icon: {icon}')
    if colour is not None:
        lines.append(f'colour: #{colour:06X}')
    return '\n'.join(lines) + '\n'


def find_marker_files(markers_dir):
    """key -> Path for every markers/*.txt. Two files with the same key is an error."""
    found = {}
    if not markers_dir.is_dir():
        return found
    for p in sorted(markers_dir.glob('*.txt')):
        key = tune_key(p.stem)
        if key in found:
            raise SyncError(f'two marker files for the same tune: {found[key].name} and {p.name}')
        found[key] = p
    return found


def write_new_file(path, text):
    """Create path; raises FileExistsError if it exists. sync-tunes never overwrites a marker file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'x', encoding='utf-8') as f:
        f.write(text)


# ---------------------------------------------------------------- play-1 start proposal

ENV_STEP_S = 0.05


def loudness_envelope(path):
    """RMS level in dB for each 50 ms of the recording (mono, 8 kHz), via ffmpeg astats."""
    n = int(8000 * ENV_STEP_S)
    af = (f'aformat=channel_layouts=mono,aresample=8000,asetnsamples=n={n}:p=0,'
          'astats=metadata=1:reset=1:measure_overall=RMS_level:measure_perchannel=none,'
          'ametadata=print:key=lavfi.astats.Overall.RMS_level:file=-')
    out = run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-af', af, '-f', 'null', '-'])
    env = []
    for m in re.finditer(r'RMS_level=(\S+)', out):
        v = m.group(1)
        env.append(-120.0 if v in ('-inf', 'inf', 'nan') else max(-120.0, float(v)))
    return env


def propose_play1_start(env, step_s=ENV_STEP_S, sustain_s=2.0, active_frac=0.9, below_median_db=15.0,
                        lead_s=0.1):
    """First sustained sound: the first frame that starts a 2 s window which is >= 90 % 'active' (louder than the
    music's median minus 15 dB). Lead-in silence and count-in clicks never fill 90 % of a window. Returns ms, rounded
    to 0.1 s and moved 0.1 s earlier so the first note's attack isn't clipped; None if nothing qualifies."""
    loud = sorted(x for x in env if x > -50.0)
    if not loud:
        return None
    threshold = loud[len(loud) // 2] - below_median_db
    active = [x > threshold for x in env]
    w = max(1, int(round(sustain_s / step_s)))
    need = active_frac * w
    count = sum(active[:w])
    for i in range(0, len(env) - w + 1):
        if i:
            count += active[i + w - 1] - active[i - 1]
        if active[i] and count >= need:
            ms = max(0.0, i * step_s - lead_s) * 1000
            return int(round(ms / 100)) * 100
    return None


# ---------------------------------------------------------------- later plays (tempo changes)
# The plays follow each other without a pause, so they are found by tempo, not loudness: each play is the same tune,
# faster. Measured on the 16 hand-checked marker files (2026-11-26): every real play change was >= 1.26x faster, and
# the changes found are a median 2 s after the hand-set starts (the pick-up into the next play). With that shift: play
# count right for 15 of 16 tunes; 20 of 28 later starts within 2 s, 22 within 3 s, worst 11.4 s. So it is a proposal:
# check each speed on the board and fix a start with ./mark.

ONSET_HOP_S = 256 / 16000      # ffmpeg aspectralstats hop at 16 kHz, window 512
PLAY_WIN_S = 6.0               # rhythm fingerprint window ...
PLAY_STEP_S = 0.5              # ... and step
PLAY_MIN_S = 15.0              # a play is at least this long
PLAY_LEAD_S = 2.0              # move each detected change this much earlier
PLAY_MIN_SPEEDUP = 1.15        # neighbours closer in tempo than this are one play
MAX_PROPOSED_PLAYS = 3


def onset_strength(path):
    """Note-onset strength every ONSET_HOP_S: ffmpeg's spectral flux minus its 1 s running mean, negatives dropped."""
    af = ('aformat=channel_layouts=mono,aresample=16000,aspectralstats=win_size=512:measure=flux,'
          'ametadata=print:key=lavfi.aspectralstats.1.flux:file=-')
    out = run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-af', af, '-f', 'null', '-'])
    flux = [float(v) if v not in ('nan', 'inf', '-inf') else 0.0 for v in re.findall(r'flux=(\S+)', out)]
    return novelty(flux)


def novelty(flux, avg_s=0.5):
    w = max(1, int(avg_s / ONSET_HOP_S))
    pre = [0.0]
    for x in flux:
        pre.append(pre[-1] + x)
    out = []
    for i, x in enumerate(flux):
        a, b = max(0, i - w), min(len(flux), i + w + 1)
        out.append(max(0.0, x - (pre[b] - pre[a]) / (b - a)))
    return out


def _autocorr(seg, l0, l1, stride=1):
    mu = sum(seg) / len(seg)
    seg = [x - mu for x in seg]
    e = sum(x * x for x in seg) or 1.0
    n = len(seg)
    return [sum(seg[i] * seg[i + lag] for i in range(n - lag)) / e for lag in range(l0, min(l1, n - 1), stride)]


def rhythm_signatures(o, start_s=0.0, win_s=PLAY_WIN_S, step_s=PLAY_STEP_S, lag_min=0.15, lag_max=1.6):
    """One autocorrelation curve (beat lengths lag_min..lag_max) per window, from start_s on."""
    W, l0, l1 = int(win_s / ONSET_HOP_S), int(lag_min / ONSET_HOP_S), int(lag_max / ONSET_HOP_S)
    first, step = int(start_s / ONSET_HOP_S), max(1, int(step_s / ONSET_HOP_S))
    return [_autocorr(o[a:a + W], l0, l1, 2) for a in range(first, len(o) - W, step)]


def split_segments(sigs, k, min_len):
    """Split the window sequence into k runs (each >= min_len windows) with the least spread around each run's mean
    fingerprint (dynamic programming). Returns (the k-1 boundary indexes, cost)."""
    n = len(sigs)
    if k == 1 or n < k * min_len:
        return [], None
    d = len(sigs[0])
    P, Q = [[0.0] * d], [0.0]
    for v in sigs:
        P.append([a + b for a, b in zip(P[-1], v)])
        Q.append(Q[-1] + sum(x * x for x in v))

    def sse(a, b):
        return (Q[b] - Q[a]) - sum((y - x) ** 2 for x, y in zip(P[a], P[b])) / (b - a)

    INF = float('inf')
    cost = [[INF] * (n + 1) for _ in range(k + 1)]
    back = [[0] * (n + 1) for _ in range(k + 1)]
    cost[0][0] = 0.0
    for j in range(1, k + 1):
        for b in range(j * min_len, n + 1):
            for a in range((j - 1) * min_len, b - min_len + 1):
                if cost[j - 1][a] < INF:
                    c = cost[j - 1][a] + sse(a, b)
                    if c < cost[j][b]:
                        cost[j][b], back[j][b] = c, a
    bounds, b = [], n
    for j in range(k, 0, -1):
        b = back[j][b]
        bounds.append(b)
    return sorted(bounds)[1:], cost[k][n]


def tempo_ratio(o, a_s, b_s, c_s, lag_min=0.15, lag_max=1.6):
    """How much faster [b_s, c_s) is than [a_s, b_s): the time scale that best maps the first stretch's
    autocorrelation onto the second's."""
    l0, l1 = int(lag_min / ONSET_HOP_S), int(lag_max / ONSET_HOP_S)
    h = ONSET_HOP_S
    r1 = _autocorr(o[int(a_s / h):int(b_s / h)], l0, l1)
    r2 = _autocorr(o[int(b_s / h):int(c_s / h)], l0, l1)
    best = (-2.0, 1.0)
    for s100 in range(90, 222, 2):
        s = s100 / 100
        num = d1 = d2 = 0.0
        for j, v in enumerate(r1):
            x = (j + l0) / s - l0
            i = int(x)
            if 0 <= i < len(r2) - 1:
                w = r2[i] + (x - i) * (r2[i + 1] - r2[i])
                num, d1, d2 = num + v * w, d1 + v * v, d2 + w * w
        c = num / ((d1 * d2) ** 0.5 or 1.0)
        if c > best[0]:
            best = (c, s)
    return best[1]


def propose_play_starts(o, play1_ms, max_plays=MAX_PROPOSED_PLAYS):
    """Start of each play in ms (play 1 first): split into max_plays by rhythm, merge neighbours that are not clearly
    faster, split again with that many, move each change PLAY_LEAD_S earlier, round to 0.1 s."""
    start = play1_ms / 1000
    sigs = rhythm_signatures(o, start)
    min_len = int(PLAY_MIN_S / PLAY_STEP_S)
    end = len(o) * ONSET_HOP_S

    def times(k):
        bounds, _ = split_segments(sigs, k, min_len)
        return [start + b * PLAY_STEP_S + PLAY_WIN_S / 2 - PLAY_LEAD_S for b in bounds]

    k = max_plays
    while k > 1:
        t = times(k)
        if len(t) < k - 1:
            k -= 1
            continue
        edges = [start] + t + [end]
        if all(tempo_ratio(o, edges[i], edges[i + 1], edges[i + 2]) >= PLAY_MIN_SPEEDUP for i in range(k - 1)):
            break
        k -= 1
    starts = [start] + (times(k) if k > 1 else [])
    return [int(round(x * 10)) * 100 for x in starts]


# ---------------------------------------------------------------- MP3 frames

_BITRATES_V1_L3 = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0)
_RATES_V1 = (44100, 48000, 32000, 0)
MP3_SAMPLES_PER_FRAME = 1152  # MPEG-1 Layer III


@dataclass
class Mp3Header:
    length: int
    side_info: int
    crc: int
    sample_rate: int
    channels: int
    bitrate_kbps: int


def parse_mp3_header(b):
    """4 header bytes -> Mp3Header for MPEG-1 Layer III, else None."""
    if len(b) < 4 or b[0] != 0xFF or (b[1] & 0xE0) != 0xE0:
        return None
    version, layer = (b[1] >> 3) & 3, (b[1] >> 1) & 3
    if version != 3 or layer != 1:          # 3 = MPEG-1, 1 = Layer III
        return None
    crc = 0 if (b[1] & 1) else 2
    br, sr_i, pad = _BITRATES_V1_L3[b[2] >> 4], _RATES_V1[(b[2] >> 2) & 3], (b[2] >> 1) & 1
    if br == 0 or sr_i == 0:
        return None
    channels = 1 if (b[3] >> 6) == 3 else 2
    return Mp3Header(length=144000 * br // sr_i + pad, side_info=17 if channels == 1 else 32, crc=crc,
                     sample_rate=sr_i, channels=channels, bitrate_kbps=br)


@dataclass
class Mp3Stream:
    sample_rate: int
    channels: int
    bitrate_kbps: int
    delay: int                 # decoded samples to skip at the start (LAME encoder delay + decoder delay)
    total_samples: int         # playable samples
    audio: bytes               # the audio frames only (no ID3, no Info frame): this is the file on the device
    offsets: list = field(default_factory=list)          # byte offset of each frame within `audio`
    main_data_begin: list = field(default_factory=list)  # bit-reservoir back-pointer of each frame
    main_data_len: list = field(default_factory=list)    # bytes of main data each frame carries


def _lame_delay_padding(frame, hdr):
    """Encoder delay and padding from an Info/Xing frame's LAME extension, or None if the frame isn't one."""
    p = 4 + hdr.crc + hdr.side_info
    tag = frame[p:p + 4]
    if tag not in (b'Info', b'Xing'):
        return None
    flags = struct.unpack('>I', frame[p + 4:p + 8])[0]
    q = p + 8 + (4 if flags & 1 else 0) + (4 if flags & 2 else 0) + (100 if flags & 4 else 0) + (4 if flags & 8 else 0)
    ext = frame[q:q + 36]
    if len(ext) < 24:
        raise SyncError('MP3 Info frame has no LAME extension (encoder delay unknown)')
    d = ext[21:24]
    return (d[0] << 4) | (d[1] >> 4), ((d[1] & 0x0F) << 8) | d[2]


def parse_mp3(data):
    """Walk an MPEG-1 Layer III stream written by ffmpeg/LAME: first frame must be the Info frame (gives delay and
    padding), every other frame must be audio of one sample rate / channel count. Trailing ID3v1 is allowed."""
    pos = 0
    if data[:3] == b'ID3':
        size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
        pos = 10 + size
    hdr = parse_mp3_header(data[pos:pos + 4])
    if hdr is None:
        raise SyncError('not an MPEG-1 Layer III stream')
    lame = _lame_delay_padding(data[pos:pos + hdr.length], hdr)
    if lame is None:
        raise SyncError('MP3 has no Info/LAME frame (encoder delay unknown)')
    enc_delay, enc_padding = lame
    pos += hdr.length
    first = parse_mp3_header(data[pos:pos + 4])
    if first is None:
        raise SyncError('no audio frame after the Info frame')
    start = pos
    s = Mp3Stream(sample_rate=first.sample_rate, channels=first.channels, bitrate_kbps=first.bitrate_kbps,
                  delay=enc_delay + MP3_DECODER_DELAY, total_samples=0, audio=b'')
    while pos < len(data):
        if data[pos:pos + 3] == b'TAG' and len(data) - pos == 128:
            break
        h = parse_mp3_header(data[pos:pos + 4])
        if h is None:
            raise SyncError(f'bad MP3 frame header at byte {pos}')
        if (h.sample_rate, h.channels) != (s.sample_rate, s.channels):
            raise SyncError(f'MP3 format changes at byte {pos}')
        if pos + h.length > len(data):
            raise SyncError(f'truncated MP3 frame at byte {pos}')
        si = data[pos + 4 + h.crc:pos + 4 + h.crc + 2]
        s.offsets.append(pos - start)
        s.main_data_begin.append((si[0] << 1) | (si[1] >> 7))
        s.main_data_len.append(h.length - 4 - h.crc - h.side_info)
        pos += h.length
    s.audio = data[start:pos]
    n = len(s.offsets)
    s.total_samples = n * MP3_SAMPLES_PER_FRAME - s.delay - max(0, enc_padding - MP3_DECODER_DELAY)
    if s.total_samples <= 0:
        raise SyncError('MP3 has no playable samples')
    return s


def decode_start_frame(stream, k):
    """Earliest frame to feed the decoder so that frame k decodes exactly: the frames holding k's bit-reservoir
    bytes, plus one more for the IMDCT overlap with the previous granule. Output of frames before k is discarded."""
    need, have, j = stream.main_data_begin[k], 0, k
    while have < need:
        j -= 1
        if j < 0:
            raise SyncError(f'MP3 frame {k} points before the start of the stream')
        have += stream.main_data_len[j]
    return max(0, j - 1)


def seconds_table(stream):
    """For each second s: (byte offset, frame) where decoding must start so that ANY sample in [s, s+1) can be
    reached exactly (decode from there, discard output up to the target). Timeline: sample t of the tune is decoded
    sample t + delay."""
    spf, sr = MP3_SAMPLES_PER_FRAME, stream.sample_rate
    n_frames = len(stream.offsets)
    table = []
    for s in range(math.ceil(stream.total_samples / sr)):
        first = (s * sr + stream.delay) // spf
        last = min(n_frames - 1, (min((s + 1) * sr, stream.total_samples) - 1 + stream.delay) // spf)
        f = min(decode_start_frame(stream, k) for k in range(first, last + 1))
        table.append((stream.offsets[f], f))
    return table


# ---------------------------------------------------------------- INDEX.BIN

@dataclass
class IndexTune:
    tune_id: int
    title: str
    file_name: str
    sample_rate: int
    channels: int
    total_samples: int
    file_bytes: int
    delay: int
    samples_per_frame: int
    markers_ms: list
    seconds: list            # [(byte offset, frame)] one per second
    icon: int = 0xF001       # FontAwesome codepoint (tools/icons.txt)
    colour: int = 0xF2A33A   # 0xRRGGBB


def write_index(tunes):
    """Little-endian. Header (16 B) | records | CRC32 (zlib) of everything before it.
    Record: u32 record_len, u32 tune_id, u32 sample_rate, u32 total_samples, u32 file_bytes, u16 delay,
    u16 samples_per_frame, u16 seconds_count, u8 channels, u8 marker_count, u8 file_name_len, u8 title_len,
    u16 icon (FontAwesome codepoint), u32 colour (0x00RRGGBB), file name (ASCII), title (UTF-8),
    u32 marker ms x marker_count, {u32 offset, u32 frame} x seconds. (v1 had u16 reserved and no colour.)"""
    body = b''
    for t in tunes:
        name, title = t.file_name.encode('ascii'), t.title.encode('utf-8')
        if (len(title) > MAX_TITLE_BYTES or len(name) > 12 or len(t.markers_ms) > MAX_MARKERS
                or not 0xE000 <= t.icon <= 0xF8FF or not 0 <= t.colour <= 0xFFFFFF):
            raise SyncError(f'{t.title}: title, file name or marker list too long for the index')
        tail = name + title + b''.join(struct.pack('<I', m) for m in t.markers_ms)
        tail += b''.join(INDEX_SECOND.pack(off, fr) for off, fr in t.seconds)
        rec_len = INDEX_RECORD.size + len(tail)
        body += INDEX_RECORD.pack(rec_len, t.tune_id, t.sample_rate, t.total_samples, t.file_bytes, t.delay,
                                  t.samples_per_frame, len(t.seconds), t.channels, len(t.markers_ms), len(name),
                                  len(title), t.icon, t.colour) + tail
    total = INDEX_HEADER.size + len(body) + 4
    data = INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, len(tunes), CODEC_MP3, total) + body
    return data + struct.pack('<I', zlib.crc32(data))


def read_index(data):
    """Inverse of write_index (used by tests and as a self-check before flashing)."""
    if len(data) < INDEX_HEADER.size + 4 or zlib.crc32(data[:-4]) != struct.unpack('<I', data[-4:])[0]:
        raise SyncError('index: bad size or CRC')
    magic, version, count, codec, total = INDEX_HEADER.unpack_from(data)
    if magic != INDEX_MAGIC or version != INDEX_VERSION or codec != CODEC_MP3 or total != len(data):
        raise SyncError('index: bad header')
    pos, tunes = INDEX_HEADER.size, []
    for _ in range(count):
        (rec_len, tid, sr, total_samples, file_bytes, delay, spf, n_sec, ch, n_mark, name_len, title_len,
         icon, colour) = INDEX_RECORD.unpack_from(data, pos)
        p = pos + INDEX_RECORD.size
        name = data[p:p + name_len].decode('ascii'); p += name_len
        title = data[p:p + title_len].decode('utf-8'); p += title_len
        markers = list(struct.unpack_from(f'<{n_mark}I', data, p)); p += 4 * n_mark
        seconds = [INDEX_SECOND.unpack_from(data, p + 8 * i) for i in range(n_sec)]; p += 8 * n_sec
        if p - pos != rec_len:
            raise SyncError('index: record length mismatch')
        tunes.append(IndexTune(tid, title, name, sr, ch, total_samples, file_bytes, delay, spf, markers, seconds,
                               icon, colour))
        pos = p
    if pos != len(data) - 4:
        raise SyncError('index: trailing bytes')
    return tunes


def fixture_tunes():
    """Fixed data for tests/host/fixtures/index_v2.bin, which the C index parser (components/logic) also reads."""
    return [
        IndexTune(tune_id('Clare Jig'), 'Clare Jig', 'T01.MP3', 32000, 1, 96000, 432 * 2, 1105, 1152,
                  [500, 2000], [(0, 0), (144, 1), (288, 2)], icon=0xF06C, colour=0x2EC4B6),
        IndexTune(tune_id('Nóra Chríonna'), 'Nóra Chríonna', 'T02.MP3', 32000, 1, 16000, 288, 1105, 1152,
                  [], [(0, 0)], icon=0xF186, colour=0xB388FF),
    ]


# ---------------------------------------------------------------- partition and FAT image sizing

def partition_size(csv_path, name):
    """Size in bytes of partition `name`, read with IDF's gen_esp32part (the same parser the build uses)."""
    sys.path.insert(0, str(Path(os.environ['IDF_PATH']) / 'components' / 'partition_table'))
    import gen_esp32part  # noqa: E402  (IDF tool, only importable inside the IDF env)
    gen_esp32part.quiet = True
    with open(csv_path) as f:
        table = gen_esp32part.PartitionTable.from_csv(f.read())
    p = table.find_by_name(name)
    if p is None:
        raise SyncError(f'no partition named {name!r} in {csv_path}')
    return p.size


def fat_data_bytes(part_bytes, sector=SECTOR, fat_count=2, root_entries=ROOT_ENTRIES, sectors_per_cluster=1):
    """Bytes available for file data in a fatfsgen image: total minus boot sector, FATs and root directory."""
    total = part_bytes // sector
    root = math.ceil(root_entries * 32 / sector)
    clusters = (total - 1 - root) // sectors_per_cluster
    fat_bytes = math.ceil((clusters + 2) * 1.5) if clusters < FAT12_MAX_CLUSTERS else (clusters + 2) * 2
    fats = fat_count * math.ceil(fat_bytes / sector)
    return (total - 1 - root - fats) // sectors_per_cluster * sectors_per_cluster * sector


FAT16_MIN_CLUSTERS = 4400   # FatFs: more than 4085 data clusters = FAT16; stay well clear of the edge
FAT16_MAX_CLUSTERS = 65000  # FatFs: at most 65525


def sd_layout(file_sizes, sector=SD_SECTOR):
    """(data clusters, sectors per cluster) of a FAT16 volume just big enough for these files plus 1% (at least
    1 MB): the smallest cluster size that stays inside FAT16."""
    for spc in (1, 2, 4, 8, 16, 32, 64):
        cluster = spc * sector
        need = sum(math.ceil(n / cluster) for n in file_sizes)
        clusters = max(need + max(need // 100, math.ceil((1 << 20) / cluster)), FAT16_MIN_CLUSTERS)
        if clusters <= FAT16_MAX_CLUSTERS:
            return clusters, spc
    raise SyncError(f'{mb(sum(file_sizes))} of tunes is more than a FAT16 volume (2 GB) can hold')


def _dos_name(name):
    base, _, ext = name.upper().partition('.')
    if not (1 <= len(base) <= 8 and len(ext) <= 3 and re.fullmatch(r'[A-Z0-9_]+', base + ext or '_')):
        raise ValueError(f'not an 8.3 name: {name!r}')
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def fat16_image(files, clusters, spc, when=None, volume_id=0x54554E45, sector=SD_SECTOR, root_entries=ROOT_ENTRIES):
    """A FAT16 volume (no partition table) holding `files` [(8.3 name, bytes)] in the root directory, each stored
    contiguously. fatfsgen cannot be used for the card: it sizes the FAT from the total sector count, so with
    512-byte sectors (which FatFs needs on an SD card) it stops at about 32 MB."""
    when = when or datetime.datetime.now()
    dos_date = ((when.year - 1980) << 9) | (when.month << 5) | when.day
    dos_time = (when.hour << 11) | (when.minute << 5) | (when.second // 2)
    fat_sectors = math.ceil((clusters + 2) * 2 / sector)
    root_sectors = math.ceil(root_entries * 32 / sector)
    data_start = 1 + 2 * fat_sectors + root_sectors
    total = data_start + clusters * spc
    if len(files) + 1 > root_entries:
        raise SyncError(f'{len(files)} files: the root directory holds {root_entries - 1}')

    boot = bytearray(sector)
    boot[0:3] = b'\xEB\x3C\x90'
    boot[3:11] = b'MSDOS5.0'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, sector, spc, 1, 2, root_entries, total if total < 0x10000 else 0,
                     0xF8, fat_sectors, 63, 255, 0, total if total >= 0x10000 else 0)
    struct.pack_into('<BBBI11s8s', boot, 36, 0x80, 0, 0x29, volume_id, b'TUNES      ', b'FAT16   ')
    boot[510:512] = b'\x55\xAA'

    fat = bytearray(fat_sectors * sector)
    struct.pack_into('<HH', fat, 0, 0xFFF8, 0xFFFF)
    root = bytearray(root_sectors * sector)
    root[0:11] = b'TUNES      '
    root[11] = 0x08                                   # volume label
    struct.pack_into('<HH', root, 22, dos_time, dos_date)
    data = bytearray(clusters * spc * sector)
    cluster_bytes = spc * sector
    nxt = 2
    for i, (name, content) in enumerate(files, 1):
        n = math.ceil(len(content) / cluster_bytes)
        if nxt - 2 + n > clusters:
            raise SyncError('the files do not fit the volume')
        first = nxt if n else 0
        for c in range(nxt, nxt + n):
            struct.pack_into('<H', fat, 2 * c, c + 1 if c < nxt + n - 1 else 0xFFFF)
        off = (nxt - 2) * cluster_bytes
        data[off:off + len(content)] = content
        e = 32 * i
        root[e:e + 11] = _dos_name(name)
        root[e + 11] = 0x20                           # archive
        struct.pack_into('<HHHHHHHI', root, e + 14, dos_time, dos_date, dos_date, 0, dos_time, dos_date, first,
                         len(content))
        nxt += n
    return bytes(boot + fat + fat + root + data)


def on_disk(nbytes, sector=SECTOR):
    return math.ceil(nbytes / sector) * sector


# ---------------------------------------------------------------- helpers

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise SyncError(f'{Path(cmd[0]).name} failed ({r.returncode}):\n{(r.stderr or r.stdout).strip()[-2000:]}')
    return r.stdout


def source_duration_s(path):
    return float(run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0',
                      str(path)]).strip())


def encode(src, cache_dir, bitrate_kbps=FLASH_BITRATE_KBPS):
    """recording -> MP3 bytes (cached by content + settings)."""
    settings = f'mp3 cbr {bitrate_kbps}k {SAMPLE_RATE}Hz {CHANNELS}ch v1'
    h = hashlib.sha256(settings.encode())
    with open(src, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    out = cache_dir / f'{h.hexdigest()[:24]}.mp3'
    if not out.exists():
        cache_dir.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix('.tmp.mp3')
        run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', str(src), '-vn', '-ac', str(CHANNELS),
             '-ar', str(SAMPLE_RATE), '-c:a', 'libmp3lame', '-b:a', f'{bitrate_kbps}k',
             '-id3v2_version', '0', '-write_id3v1', '0', '-map_metadata', '-1', str(tmp)])
        tmp.rename(out)
    return out.read_bytes()


@dataclass
class Tune:
    src: Path
    title: str
    tune_id: int
    stream: Mp3Stream = None
    markers_ms: list = None
    icon: str = DEFAULT_ICON
    colour: int = None
    marker_file: Path = None


def scan(recordings_dir):
    if not recordings_dir.is_dir():
        raise SyncError(f'{recordings_dir} not found')
    tunes, ignored = [], []
    for p in sorted(recordings_dir.iterdir()):
        if p.is_file() and p.suffix.lower() in RECORDING_EXTS:
            title = tune_title(p.stem)
            if not title:
                raise SyncError(f'{p.name}: empty title')
            if '/' in title:
                raise SyncError(f'{p.name}: "/" not allowed in a title')
            tunes.append(Tune(src=p, title=title, tune_id=tune_id(title)))
        elif p.is_file() and not p.name.startswith('.'):
            ignored.append(p.name)
    seen = {}
    for t in tunes:
        if t.tune_id in seen:
            raise SyncError(f'"{t.title}" and "{seen[t.tune_id]}" give the same tune ID; rename one')
        if t.tune_id == 0:
            raise SyncError(f'"{t.title}": tune ID is 0 (reserved); rename it')
        seen[t.tune_id] = t.title
    tunes.sort(key=lambda t: tune_key(t.title))
    return tunes, ignored


def mb(n):
    return f'{n / 1e6:.2f} MB'


def minutes_at_bitrate(nbytes, kbps):
    return nbytes / (kbps * 1000 / 8) / 60


def write_card(image, tune_count, wipe):
    """Write a FAT image to the board's SD card through the running firmware (tools/board_link.py); see step 5."""
    import monitor  # pyserial: only inside the IDF env
    import board_link as bl
    link = bl.Link(monitor)
    try:
        port = link.open()
        inf = link.info()
        if inf.version != bl.VERSION:
            raise SyncError(f'board firmware speaks link version {inf.version}, this script {bl.VERSION}: run ./flash')
        if not inf.card_sectors:
            raise SyncError('the board found no SD card. To use the built-in storage instead: ./sync-tunes --built-in')
        card = inf.card_sectors * bl.SECTOR
        if len(image) > card:
            raise SyncError(f'tunes need {mb(len(image))}, the SD card holds {mb(card)}')
        if inf.mode == bl.MODE_PLAYER and inf.source != 2 and not wipe:
            raise SyncError('the SD card does not hold tunes yet (a new card, or the last sync was interrupted). '
                            'Writing ERASES EVERYTHING on it. If that is OK: ./sync-tunes --wipe-card')
        print(f'sync-tunes: writing {mb(len(image))} to the SD card ({mb(card)}) via {port}. '
              'The board shows "Syncing tunes"; keep the cable in.')
        if inf.mode != bl.MODE_SYNC:
            link.restart_into('B', 'b', bl.MODE_SYNC)
        tty = sys.stdout.isatty()
        last = [-1]

        def progress(done, total, secs):
            pct = done * 100 // total
            if pct != last[0] and (tty or pct % 10 == 0):
                last[0] = pct
                rate = done * bl.SECTOR / max(secs, 1e-3) / 1e6
                print(f'{chr(13) if tty else ""}  {pct:3d}%  {mb(done * bl.SECTOR)}  {rate:.2f} MB/s',
                      end='' if tty and pct < 100 else '\n', flush=True)

        secs = bl.write_image(link, image, progress)
        inf = link.restart_into('X', 'x', bl.MODE_PLAYER)
        end = time.time() + 10   # the player loads the index a moment after the board answers
        while inf.tune_count != tune_count and time.time() < end:
            time.sleep(0.5)
            inf = link.info()
        print(f'sync-tunes: written and read back in {secs:.0f} s; the board now plays {inf.tune_count} tunes from '
              f'the {bl.SOURCES.get(inf.source, "?")}')
        if inf.source != 2 or inf.tune_count != tune_count:
            raise SyncError(f'expected {tune_count} tunes from the SD card. Run ./sync-tunes again')
        return card
    except bl.LinkError as e:
        raise SyncError(f'{e}. Nothing is lost: run ./sync-tunes again (until then the board plays its built-in '
                        'tunes)') from None
    finally:
        link.close()


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(prog='sync-tunes', description=__doc__.split('\n\n')[0])
    ap.add_argument('--no-flash', action='store_true', help='do everything except writing to the board')
    ap.add_argument('--built-in', action='store_true',
                    help=f'write the built-in flash `tunes` partition ({FLASH_BITRATE_KBPS} kbps) instead of the SD '
                         f'card ({SD_BITRATE_KBPS} kbps)')
    ap.add_argument('--wipe-card', action='store_true',
                    help='allow erasing an SD card that does not hold tunes yet (a new card)')
    ap.add_argument('--write-test-fixture', metavar='PATH', help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.write_test_fixture:
        Path(args.write_test_fixture).write_bytes(write_index(fixture_tunes()))
        return 0

    to_card = not args.built_in
    kbps = SD_BITRATE_KBPS if to_card else FLASH_BITRATE_KBPS
    tunes, ignored = scan(RECORDINGS)
    if not tunes:
        raise SyncError(f'no {"/".join(RECORDING_EXTS)} files in {RECORDINGS}')
    for name in ignored:
        print(f'note: ignoring {name} (not {"/".join(RECORDING_EXTS)})')

    # 1. encode + parse
    for t in tunes:
        t.stream = parse_mp3(encode(t.src, BUILD / 'cache', kbps))
        if (t.stream.sample_rate, t.stream.channels, t.stream.bitrate_kbps) != (SAMPLE_RATE, CHANNELS, kbps):
            raise SyncError(f'{t.title}: encoder produced an unexpected format')
        src_s, got_s = source_duration_s(t.src), t.stream.total_samples / SAMPLE_RATE
        if abs(src_s - got_s) > 0.05:
            raise SyncError(f'{t.title}: duration {got_s:.3f} s after encoding vs {src_s:.3f} s source')

    # 2. markers, icon, colour
    icons = load_icons()
    marker_files = find_marker_files(MARKERS)
    keys = {tune_key(t.title) for t in tunes}
    today = datetime.date.today().isoformat()
    new_markers = []
    for t in tunes:
        key = tune_key(t.title)
        duration_ms = t.stream.total_samples * 1000 // SAMPLE_RATE
        if key in marker_files:
            t.marker_file = marker_files[key]
            tf = parse_tune_file(t.marker_file.read_text(encoding='utf-8'), duration_ms,
                                 f'markers/{t.marker_file.name}', icons)
            t.markers_ms = tf.starts
            t.icon = tf.icon or DEFAULT_ICON
            t.colour = tf.colour
            continue
        t.markers_ms = []
        play1 = propose_play1_start(loudness_envelope(t.src))
        if play1 is None:
            print(f'markers: "{t.title}": no sustained sound found; it will play from 0:00 (set starts: ./mark)')
            continue
        print(f'markers: "{t.title}": new recording, finding where each play starts...', flush=True)
        starts = [ms for ms in propose_play_starts(onset_strength(t.src), play1) if ms < duration_ms]
        path = MARKERS / f'{t.title}.txt'
        write_new_file(path, marker_file_text(t.title, starts, today))
        t.marker_file, t.markers_ms = path, starts
        new_markers.append(t)
    orphans = sorted(p.name for k, p in marker_files.items() if k not in keys)

    # 3. index + size check
    files = [(f'T{i:02d}.MP3', t) for i, t in enumerate(tunes, 1)]
    index = write_index([IndexTune(t.tune_id, t.title, name, SAMPLE_RATE, CHANNELS, t.stream.total_samples,
                                   len(t.stream.audio), t.stream.delay, MP3_SAMPLES_PER_FRAME, t.markers_ms,
                                   seconds_table(t.stream), icons[t.icon],
                                   t.colour if t.colour is not None else default_colour(t.tune_id))
                      for name, t in files])
    read_index(index)  # self-check
    total_min = sum(t.stream.total_samples for t in tunes) / SAMPLE_RATE / 60
    if to_card:
        clusters, spc = sd_layout([len(t.stream.audio) for _, t in files] + [len(index)])
        where = 'the SD card'
    else:
        part = partition_size(PARTITIONS_CSV, PARTITION_NAME)
        where = f'the built-in "{PARTITION_NAME}" partition'
        capacity = fat_data_bytes(part)
        used = sum(on_disk(len(t.stream.audio)) for _, t in files) + on_disk(len(index))
        if used > capacity:
            print(f'sync-tunes: REFUSED, nothing written. {len(tunes)} tunes ({total_min:.1f} min) need {mb(used)}; '
                  f'the {PARTITION_NAME} partition holds {mb(capacity)}.')
            print(f'  Over by {mb(used - capacity)} (~{minutes_at_bitrate(used - capacity, kbps):.1f} min at {kbps} '
                  'kbps). Move recordings out of recordings/ until it fits, or use the SD card. Sizes:')
            for _, t in sorted(files, key=lambda f: -len(f[1].stream.audio)):
                print(f'    {mb(on_disk(len(t.stream.audio)))}  {t.title}')
            return 2

    # 4. image
    stage = BUILD / 'image'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for name, t in files:
        (stage / name).write_bytes(t.stream.audio)
    (stage / 'INDEX.BIN').write_bytes(index)
    if to_card:
        image = BUILD / 'card.img'
        image.write_bytes(fat16_image([(name, t.stream.audio) for name, t in files] + [('INDEX.BIN', index)],
                                      clusters, spc, volume_id=zlib.crc32(index)))
        part = image.stat().st_size
    else:
        image = BUILD / 'tunes.img'
        run([sys.executable, str(Path(os.environ['IDF_PATH']) / 'components' / 'fatfs' / 'fatfsgen.py'), str(stage),
             '--output_file', str(image), '--partition_size', str(part), '--sector_size', str(SECTOR)])
        if image.stat().st_size != part:
            raise SyncError(f'image is {image.stat().st_size} bytes, partition is {part}')

    # 5. write
    if args.no_flash:
        print(f'sync-tunes: --no-flash: image at {image.relative_to(ROOT)}, board not touched')
        card = None
    elif to_card:
        card = write_card(image.read_bytes(), len(tunes), args.wipe_card)
    else:
        port = run([sys.executable, str(ROOT / 'tools' / 'monitor.py'), '--print-port']).strip()
        print(f'sync-tunes: writing {image.relative_to(ROOT)} to the "{PARTITION_NAME}" partition of the ESP32-S3 '
              f'at {port} (erase + write, ~2-3 min)')
        # --ignore-readonly: `readonly` marks the partition read-only for the firmware; this is the one tool that writes
        # it. parttool reads the partition table from the board itself, so the offset always matches the device.
        r = subprocess.run([sys.executable,
                            str(Path(os.environ['IDF_PATH']) / 'components' / 'partition_table' / 'parttool.py'),
                            '--port', port, 'write_partition', '--partition-name', PARTITION_NAME,
                            '--input', str(image), '--ignore-readonly'])  # output shown: progress + hash check
        if r.returncode != 0:
            raise SyncError(f'parttool failed ({r.returncode}); the {PARTITION_NAME} partition may be incomplete, '
                            'run ./sync-tunes --built-in again')

    # 6. report
    print(f'\nsync-tunes: {len(tunes)} tunes for {where} ({total_min:.1f} min, MP3 {kbps} kbps mono '
          f'{SAMPLE_RATE // 1000} kHz)')
    for name, t in files:
        marks = ', '.join(format_time_ms(m) for m in t.markers_ms) or '-'
        colour = t.colour if t.colour is not None else default_colour(t.tune_id)
        print(f'  {name}  {t.tune_id:08x}  {format_time_ms(t.stream.total_samples * 1000 // SAMPLE_RATE):>7}  '
              f'starts: {marks:<22} {t.icon:<9} #{colour:06X}  {t.title}')
    if to_card:
        if card:
            print(f'space: {mb(part)} used of the {mb(card)} card, {mb(card - part)} left '
                  f'(~{minutes_at_bitrate(card - part, kbps) / 60:.1f} hours at {kbps} kbps)')
        else:
            print(f'space: {mb(part)} needed on the SD card')
    else:
        print(f'space: {mb(used)} used of {mb(capacity)}, {mb(capacity - used)} left '
              f'(~{minutes_at_bitrate(capacity - used, kbps):.0f} min at {kbps} kbps)')
    if new_markers:
        print('\nmarkers: new recordings got their play starts guessed (written to markers/). On the board, tap each '
              'speed on the ring and listen; if one starts in the wrong place: ./mark "<title>", then ./sync-tunes')
        for t in new_markers:
            print(f'  {", ".join(format_time_ms(m) for m in t.markers_ms):<24} {t.title}')
    if orphans:
        print(f'\nmarkers: no recording for: {", ".join(orphans)}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SyncError as e:
        print(f'sync-tunes: error: {e}', file=sys.stderr)
        sys.exit(1)
