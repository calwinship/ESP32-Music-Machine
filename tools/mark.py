#!/usr/bin/env python3
"""Check or set where each play of a tune starts, by ear, without editing a file.

  ./mark "<title>"         for each play: hear the few seconds around its start; keep it, tap it again, or delete it
  ./mark "<title>" --all   the recording plays from the beginning; press Enter as each play starts (play 1 too)

Reads and writes markers/<title>.txt (keeping its icon and colour lines); asks before saving. If the file does not
exist yet, starts from sync-tunes' guess. Run ./sync-tunes afterwards to put the new times on the board.
Plays audio with macOS afplay; clips are cut with ffmpeg into a temporary folder.
"""
import argparse
import datetime
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sync_tunes as st  # noqa: E402

TAP_DELAY_S = 0.35     # afplay start-up + how late a press usually lands after the sound; taken off each tap
BEFORE_S, AFTER_S = 4.0, 6.0      # check clip around a start
TAP_BEFORE_S, TAP_AFTER_S = 15.0, 15.0


def tap_to_ms(clip_start_ms, elapsed_s, delay_s=TAP_DELAY_S):
    """Time in the recording of a tap `elapsed_s` after a clip from clip_start_ms began, rounded to 0.1 s."""
    return max(0, int(round((clip_start_ms / 1000 + elapsed_s - delay_s) * 10)) * 100)


def fits(ms, prev_ms, next_ms, duration_ms, gap_ms=5000):
    """A start must be at least gap_ms after the previous one, before the next one and inside the recording."""
    return (prev_ms is None or ms >= prev_ms + gap_ms) and (next_ms is None or ms <= next_ms - gap_ms) \
        and 0 <= ms < duration_ms


class Player:
    def __init__(self, src, tmp):
        self.src, self.tmp, self.n = src, Path(tmp), 0

    def start(self, from_ms, length_s):
        """Cut [from_ms, from_ms + length_s) to a WAV and start playing it; returns (process, monotonic start)."""
        self.n += 1
        wav = self.tmp / f'clip{self.n}.wav'
        st.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-ss', f'{from_ms / 1000:.3f}', '-t', f'{length_s:.3f}',
                '-i', str(self.src), '-ac', '1', str(wav)])
        return subprocess.Popen(['afplay', str(wav)]), time.monotonic()

    def play(self, from_ms, length_s):
        p, _ = self.start(from_ms, length_s)
        try:
            p.wait()
        except KeyboardInterrupt:
            p.terminate()
            print()


def ask(prompt):
    try:
        return input(prompt).strip().lower()
    except EOFError:
        return 'q'


def tap(player, around_ms, label, prev_ms, next_ms, duration_ms):
    """Play the stretch around a start; the Enter press marks it. Returns the new time or None."""
    lo = max(0, around_ms - int(TAP_BEFORE_S * 1000), (prev_ms or 0) + 1000)
    length = min(TAP_BEFORE_S + TAP_AFTER_S, (duration_ms - lo) / 1000)
    print(f'  Playing from {st.format_time_ms(lo)}. Press Enter the moment {label} starts.')
    p, t0 = player.start(lo, length)
    try:
        input()
    except (EOFError, KeyboardInterrupt):
        p.terminate()
        return None
    elapsed = time.monotonic() - t0
    p.terminate()
    if elapsed > length:
        print('  That was after the clip ended; try again.')
        return None
    ms = tap_to_ms(lo, elapsed)
    if not fits(ms, prev_ms, next_ms, duration_ms):
        print(f'  {st.format_time_ms(ms)} is too close to another play or outside the recording; try again.')
        return None
    return ms


def check_each(player, starts, duration_ms):
    starts = list(starts)
    i = 0
    while i < len(starts):
        prev_ms = starts[i - 1] if i else None
        next_ms = starts[i + 1] if i + 1 < len(starts) else None
        label = f'play {i + 1}'
        frm = max(0, starts[i] - int(BEFORE_S * 1000))
        print(f'\n{label.capitalize()} of {len(starts)} is marked at {st.format_time_ms(starts[i])}. Listening from '
              f'{st.format_time_ms(frm)}: it should start about {(starts[i] - frm) / 1000:.0f} s in.')
        player.play(frm, BEFORE_S + AFTER_S)
        a = ask('  Enter = right   t = tap it again   r = replay   d = delete this play   q = stop  > ')
        if a == '':
            i += 1
        elif a == 't':
            ms = tap(player, starts[i], label, prev_ms, next_ms, duration_ms)
            if ms is not None:
                print(f'  {label.capitalize()}: {st.format_time_ms(starts[i])} -> {st.format_time_ms(ms)}')
                starts[i] = ms
        elif a == 'd':
            if len(starts) == 1:
                print('  A tune needs at least one start.')
            else:
                print(f'  Deleted {label} ({st.format_time_ms(starts.pop(i))}).')
        elif a == 'q':
            break
    return starts


def tap_all(player, duration_ms):
    print('\nThe recording plays from the beginning. Press Enter at the start of each play (play 1 too).\n'
          'After the last one, type q and Enter (or wait for the end and press q).')
    p, t0 = player.start(0, duration_ms / 1000)
    taps = []
    try:
        while True:
            line = input()
            elapsed = time.monotonic() - t0
            if line.strip().lower() == 'q':
                break
            ms = tap_to_ms(0, elapsed)
            if ms >= duration_ms or (taps and ms < taps[-1] + 5000):
                print(f'  ignored ({st.format_time_ms(ms)}: too close to the last one or past the end)')
                continue
            taps.append(ms)
            print(f'  play {len(taps)}: {st.format_time_ms(ms)}')
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        p.terminate()
    return taps


def find_recording(title):
    key = st.tune_key(title)
    for f in sorted(st.RECORDINGS.glob('*.m4a')):
        if st.tune_key(st.tune_title(f.stem)) == key:
            return f
    for f in sorted((st.RECORDINGS / 'not-synced').glob('*.m4a')):
        if st.tune_key(st.tune_title(f.stem)) == key:
            raise st.SyncError(f'"{f.name}" is in recordings/not-synced/. Move it into recordings/ first.')
    names = ', '.join(sorted(st.tune_title(f.stem) for f in st.RECORDINGS.glob('*.m4a')))
    raise st.SyncError(f'no recording called "{title}" in recordings/. There are: {names}')


def main(argv=None):
    ap = argparse.ArgumentParser(prog='mark', description=__doc__.split('\n\n')[0])
    ap.add_argument('title', help='tune title (the recording file name without .m4a)')
    ap.add_argument('--all', action='store_true', help='tap every play start while the whole recording plays')
    args = ap.parse_args(argv)

    src = find_recording(args.title)
    title = st.tune_title(src.stem)
    duration_ms = int(st.source_duration_s(src) * 1000)
    icons = st.load_icons()
    files = st.find_marker_files(st.MARKERS)
    path = files.get(st.tune_key(title), st.MARKERS / f'{title}.txt')
    icon = colour = None
    if path.exists():
        tf = st.parse_tune_file(path.read_text(encoding='utf-8'), duration_ms, f'markers/{path.name}', icons)
        old, icon, colour = tf.starts, tf.icon, tf.colour
        print(f'"{title}": markers/{path.name} has {len(old)} play(s): {", ".join(map(st.format_time_ms, old))}')
    else:
        print(f'"{title}": no markers file yet; guessing the starts...', flush=True)
        play1 = st.propose_play1_start(st.loudness_envelope(src)) or 0
        old = [m for m in st.propose_play_starts(st.onset_strength(src), play1) if m < duration_ms]
        print(f'  guess: {", ".join(map(st.format_time_ms, old))}')

    with tempfile.TemporaryDirectory() as tmp:
        player = Player(src, tmp)
        new = tap_all(player, duration_ms) if args.all else check_each(player, old, duration_ms)

    if not new:
        print('\nNo starts marked; nothing saved.')
        return 1
    if new == old and path.exists():
        print('\nNo changes.')
        return 0
    print(f'\nNew starts: {", ".join(map(st.format_time_ms, new))}')
    if ask(f'Save to markers/{path.name}? [Y/n] ') in ('', 'y', 'yes'):
        path.write_text(st.marker_file_text(title, new, datetime.date.today().isoformat(), how='set with ./mark',
                                            icon=icon, colour=colour), encoding='utf-8')
        print('Saved. Run ./sync-tunes to put it on the board.')
    else:
        print('Not saved.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except st.SyncError as e:
        print(f'mark: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('\nmark: stopped, nothing saved.')
        sys.exit(130)
