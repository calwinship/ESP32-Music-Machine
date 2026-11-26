"""Host tests for tools/sync_tunes.py (stdlib only; run by ./test). No ffmpeg, IDF or board needed."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'tools'))
import sync_tunes as st  # noqa: E402
import board_link as bl  # noqa: E402
import mark  # noqa: E402

FIXTURE = HERE / 'fixtures' / 'index_v2.bin'


class Titles(unittest.TestCase):
    def test_nfd_and_trailing_space_give_the_same_title_and_id(self):
        nfd = unicodedata.normalize('NFD', 'Nóra Chríonna')
        self.assertNotEqual(nfd, 'Nóra Chríonna')
        self.assertEqual(st.tune_title(nfd + ' '), 'Nóra Chríonna')
        self.assertEqual(st.tune_id(nfd), st.tune_id('Nóra Chríonna'))
        self.assertEqual(st.tune_title('The Wise Maid  '), 'The Wise Maid')

    def test_key_ignores_case_and_whitespace_runs(self):
        self.assertEqual(st.tune_key('  Ratlan  BOG '), 'ratlan bog')
        self.assertEqual(st.tune_id('Clare Jig'), st.tune_id('clare   jig'))
        self.assertNotEqual(st.tune_id('Clare Jig'), st.tune_id('Clare Reel'))

    def test_fnv1a_reference_vectors(self):
        self.assertEqual(st.fnv1a32(b''), 0x811C9DC5)
        self.assertEqual(st.fnv1a32(b'a'), 0xE40C292C)
        self.assertEqual(st.fnv1a32(b'foobar'), 0xBF9CF968)

    def test_id_is_pinned(self):
        # Play history will be keyed by this value: it must never change for a given title.
        self.assertEqual(st.tune_id('Clare Jig'), st.fnv1a32(b'clare jig'))

    def test_scan_refuses_two_recordings_with_the_same_id(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'Clare Jig.m4a').write_bytes(b'')
            (Path(d) / 'clare  jig .m4a').write_bytes(b'')
            with self.assertRaises(st.SyncError):
                st.scan(Path(d))

    def test_scan_orders_by_title_and_ignores_other_files(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ('b tune.m4a', 'A tune.m4a', 'notes.txt', '.DS_Store'):
                (Path(d) / n).write_bytes(b'')
            (Path(d) / 'not-synced').mkdir()
            (Path(d) / 'not-synced' / 'C tune.m4a').write_bytes(b'')
            tunes, ignored = st.scan(Path(d))
            self.assertEqual([t.title for t in tunes], ['A tune', 'b tune'])
            self.assertEqual(ignored, ['notes.txt'])


class Markers(unittest.TestCase):
    def test_times(self):
        self.assertEqual(st.parse_time_ms('1:25.4'), 85400)
        self.assertEqual(st.parse_time_ms('85.4'), 85400)
        self.assertEqual(st.parse_time_ms('0:07'), 7000)
        for bad in ('1:75', 'abc', '1:2:3', '-1', ''):
            with self.assertRaises(ValueError):
                st.parse_time_ms(bad)
        self.assertEqual(st.format_time_ms(85400), '1:25.4')
        self.assertEqual(st.format_time_ms(6800), '0:06.8')

    def test_parse_file_with_comments(self):
        text = '# header\n0:06.8  # play 1\n\n1:25.0\n2:22.3\n'
        self.assertEqual(st.parse_markers(text, 200000, 'f'), [6800, 85000, 142300])

    def test_parse_errors_name_the_line(self):
        cases = {'0:10\n0:05\n': 'f:2', '0:10\nhello\n': 'f:2', '5:00\n': 'f:1'}
        for text, where in cases.items():
            with self.assertRaises(st.SyncError) as cm:
                st.parse_markers(text, 200000, 'f')
            self.assertIn(where, str(cm.exception))
        with self.assertRaises(st.SyncError):
            st.parse_markers(''.join(f'{i}\n' for i in range(1, 10)), 200000, 'f')

    def test_proposal_file_round_trips(self):
        text = st.marker_file_text('Clare Jig', [6800, 101000, 157800], '2026-11-15')
        self.assertEqual(st.parse_markers(text, 200000, 'f'), [6800, 101000, 157800])
        self.assertIn('./mark "Clare Jig"', text)

    def test_mark_keeps_icon_and_colour(self):
        text = st.marker_file_text('Clare Jig', [6800, 99000], '2026-11-26', how='set with ./mark', icon='leaf',
                                   colour=0x2EC4B6)
        tf = st.parse_tune_file(text, 200000, 'f', st.load_icons())
        self.assertEqual((tf.starts, tf.icon, tf.colour), ([6800, 99000], 'leaf', 0x2EC4B6))
        self.assertIn('Set with ./mark on 2026-11-26', text)

    def test_never_overwrites(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'markers' / 'Clare Jig.txt'
            st.write_new_file(p, 'mine\n')
            with self.assertRaises(FileExistsError):
                st.write_new_file(p, 'proposal\n')
            self.assertEqual(p.read_text(), 'mine\n')

    def test_find_marker_files_matches_by_key_and_refuses_duplicates(self):
        with tempfile.TemporaryDirectory() as d:
            nfd = unicodedata.normalize('NFD', 'Nóra Chríonna')
            (Path(d) / f'{nfd}.txt').write_text('')
            found = st.find_marker_files(Path(d))
            self.assertIn(st.tune_key('Nóra Chríonna'), found)
            (Path(d) / 'nóra chríonna.txt').write_text('')
            if len(list(Path(d).glob('*.txt'))) == 2:  # case-insensitive filesystems merge the two names
                with self.assertRaises(st.SyncError):
                    st.find_marker_files(Path(d))


class Theme(unittest.TestCase):
    ICONS = {'leaf': 0xF06C, 'music': 0xF001}

    def test_icon_and_colour_lines(self):
        text = '# Clare Jig\n0:06.8\nicon: Leaf   # a comment\ncolour: #2ec4b6\n1:41.0\n'
        tf = st.parse_tune_file(text, 200000, 'f', self.ICONS)
        self.assertEqual((tf.starts, tf.icon, tf.colour), ([6800, 101000], 'leaf', 0x2EC4B6))
        self.assertEqual(st.parse_tune_file('color: 102030\n', 200000, 'f').colour, 0x102030)

    def test_bad_theme_lines_name_the_line(self):
        bad = {'icon: spaceship\n': 'f:1', '0:01\nicon: leaf\nicon: music\n': 'f:3', 'colour: red\n': 'f:1',
               'colour: #123\n': 'f:1', 'tempo: 120\n': 'f:1'}
        for text, where in bad.items():
            with self.assertRaises(st.SyncError) as cm:
                st.parse_tune_file(text, 200000, 'f', self.ICONS)
            self.assertIn(where, str(cm.exception))

    def test_icon_list_matches_the_font(self):
        icons = st.load_icons()
        self.assertEqual(icons['music'], 0xF001)
        self.assertIn(st.DEFAULT_ICON, icons)
        self.assertTrue(all(0xE000 <= cp <= 0xF8FF for cp in icons.values()))
        self.assertEqual(len(set(icons.values())), len(icons))

    def test_default_colour_is_stable_and_bright(self):
        c = st.default_colour(st.tune_id('Clare Jig'))
        self.assertEqual(c, st.default_colour(st.tune_id('clare  jig')))
        self.assertGreaterEqual(max(c >> 16, (c >> 8) & 255, c & 255), 230)
        self.assertNotEqual(c, st.default_colour(st.tune_id('Silver Spear')))


class Play1Proposal(unittest.TestCase):
    STEP = st.ENV_STEP_S

    def env(self, seconds, level):
        return [level] * int(round(seconds / self.STEP))

    def test_skips_lead_in_and_count_in_clicks(self):
        env = self.env(2.0, -75)
        for _ in range(4):                       # clicks: 0.15 s loud every 1.2 s
            env += self.env(0.15, -20) + self.env(1.05, -72)
        env += self.env(60.0, -19)               # music starts at 2.0 + 4 * 1.2 = 6.8 s
        self.assertEqual(st.propose_play1_start(env), 6700)

    def test_short_dips_inside_music_do_not_matter(self):
        env = self.env(3.0, -70)
        for _ in range(30):
            env += self.env(1.9, -18) + self.env(0.1, -45)
        self.assertEqual(st.propose_play1_start(env), 2900)

    def test_silence_gives_no_proposal(self):
        self.assertIsNone(st.propose_play1_start(self.env(30, -90)))


def mp3_frame(main_data_begin, bitrate_index=1, pad=b''):
    """MPEG-1 Layer III, 32 kHz, mono, no CRC; 32 kbps -> 144 bytes."""
    hdr = bytes([0xFF, 0xFB, (bitrate_index << 4) | (2 << 2), 0xC0])
    side = bytes([main_data_begin >> 1, (main_data_begin & 1) << 7]) + bytes(15)
    length = 144000 * st._BITRATES_V1_L3[bitrate_index] // 32000
    body = (hdr + side + pad)[:length]
    return body + bytes(length - len(body))


def info_frame(delay, padding):
    tag = b'Info' + struct.pack('>I', 0)       # no optional fields
    lame = b'Lavc62.28' + bytes(12) + bytes([delay >> 4, ((delay & 15) << 4) | (padding >> 8), padding & 255])
    return mp3_frame(0, bitrate_index=2, pad=tag + lame)  # 40 kbps Info frame, as ffmpeg writes


class Mp3(unittest.TestCase):
    def stream(self, mdbs, delay=576, padding=1472):
        return info_frame(delay, padding) + b''.join(mp3_frame(m) for m in mdbs)

    def test_header(self):
        h = st.parse_mp3_header(mp3_frame(0))
        self.assertEqual((h.length, h.sample_rate, h.channels, h.bitrate_kbps, h.side_info), (144, 32000, 1, 32, 17))
        self.assertIsNone(st.parse_mp3_header(b'\xff\xf3\x18\xc0'))   # MPEG-2: not supported
        self.assertIsNone(st.parse_mp3_header(b'ID3\x04'))

    def test_info_frame_is_dropped_and_timeline_matches_lame(self):
        s = st.parse_mp3(self.stream([0] * 100))
        self.assertEqual(len(s.offsets), 100)
        self.assertEqual(s.offsets[:2], [0, 144])
        self.assertEqual(len(s.audio), 100 * 144)
        self.assertEqual(s.delay, 576 + 529)
        self.assertEqual(s.total_samples, 100 * 1152 - 1105 - (1472 - 529))

    def test_id3v2_prefix_is_skipped(self):
        id3 = b'ID3\x04\x00\x00' + bytes([0, 0, 0, 10]) + bytes(10)
        self.assertEqual(len(st.parse_mp3(id3 + self.stream([0] * 10)).offsets), 10)

    def test_missing_info_frame_is_refused(self):
        with self.assertRaises(st.SyncError):
            st.parse_mp3(b''.join(mp3_frame(0) for _ in range(5)))

    def test_reservoir_pre_roll(self):
        # main data per frame = 144 - 4 - 17 = 123 bytes
        s = st.parse_mp3(self.stream([0] * 10 + [124, 0, 123, 369]))
        self.assertEqual(st.decode_start_frame(s, 0), 0)
        self.assertEqual(st.decode_start_frame(s, 5), 4)       # no reservoir: only the overlap frame before it
        self.assertEqual(st.decode_start_frame(s, 10), 7)      # 124 bytes reach into frames 9 and 8, overlap 7
        self.assertEqual(st.decode_start_frame(s, 11), 10)
        self.assertEqual(st.decode_start_frame(s, 12), 10)     # exactly one frame's 123 bytes: frame 11, overlap 10
        self.assertEqual(st.decode_start_frame(s, 13), 9)      # 369 bytes = frames 12, 11, 10; overlap 9
        with self.assertRaises(st.SyncError):
            st.decode_start_frame(st.parse_mp3(self.stream([0, 200])), 1)

    def test_seconds_table_covers_every_frame_of_each_second(self):
        s = st.parse_mp3(self.stream([0] * 200))
        table = st.seconds_table(s)
        self.assertEqual(len(table), -(-s.total_samples // 32000))
        for sec, (off, f) in enumerate(table):
            self.assertEqual(off, f * 144)
            first = (sec * 32000 + s.delay) // 1152
            self.assertEqual(f, max(0, first - 1))


class Index(unittest.TestCase):
    def test_round_trip(self):
        tunes = st.fixture_tunes()
        self.assertEqual(st.read_index(st.write_index(tunes)), tunes)

    def test_golden_fixture_is_unchanged(self):
        # The C parser in components/logic reads the same file. Regenerate only on a deliberate format change:
        #   python3 tools/sync_tunes.py --write-test-fixture tests/host/fixtures/index_v2.bin
        self.assertEqual(st.write_index(st.fixture_tunes()), FIXTURE.read_bytes())

    def test_corruption_is_detected(self):
        data = bytearray(st.write_index(st.fixture_tunes()))
        data[20] ^= 1
        with self.assertRaises(st.SyncError):
            st.read_index(bytes(data))
        with self.assertRaises(st.SyncError):
            st.read_index(bytes(data[:10]))


class Space(unittest.TestCase):
    def test_fat_overhead_for_the_tunes_partition(self):
        # 0xCF0000 / 4096 = 3312 sectors: 1 boot + 4 root dir + 2 FATs x 2 sectors (FAT12) = 9 sectors overhead
        self.assertEqual(st.fat_data_bytes(0xCF0000), (3312 - 9) * 4096)

    def test_files_round_up_to_whole_sectors(self):
        self.assertEqual(st.on_disk(1), 4096)
        self.assertEqual(st.on_disk(4096), 4096)
        self.assertEqual(st.on_disk(4097), 8192)

    def test_sd_layout_stays_inside_fat16_and_fits_the_files(self):
        for sizes in ([10], [3_000_000] * 16, [2_400_000] * 16 + [500_000], [7_000_000] * 60, [480_000_000],
                      [1_900_000_000]):
            clusters, spc = st.sd_layout(sizes)
            self.assertGreaterEqual(clusters, st.FAT16_MIN_CLUSTERS)
            self.assertLessEqual(clusters, st.FAT16_MAX_CLUSTERS)
            cluster = spc * st.SD_SECTOR
            need = sum(-(-n // cluster) for n in sizes)
            self.assertGreaterEqual(clusters, need)
            self.assertLessEqual((clusters - need) * cluster, max(sum(sizes) // 50, 3 << 20) + 4400 * cluster)
        self.assertEqual(st.sd_layout([3_000_000] * 16)[1], 2)   # 48 MB -> 1 KB clusters
        self.assertEqual(st.sd_layout([10])[1], 1)
        with self.assertRaises(st.SyncError):
            st.sd_layout([2_200_000_000])


def read_fat16(img):
    """Independent minimal FAT16 reader (root directory only): {name: bytes}, plus the cluster count FatFs sees."""
    bps, spc, rsvd, nfats, nroot, tot16, _, fatsz = struct.unpack_from('<HBHBHHBH', img, 11)
    tot = tot16 or struct.unpack_from('<I', img, 32)[0]
    assert img[510:512] == b'\x55\xaa' and img[0] == 0xEB and img[54:62] == b'FAT16   '
    root_secs = (nroot * 32 + bps - 1) // bps
    clusters = (tot - (rsvd + nfats * fatsz + root_secs)) // spc
    fat = img[rsvd * bps:(rsvd + fatsz) * bps]
    assert fat == img[(rsvd + fatsz) * bps:(rsvd + 2 * fatsz) * bps]   # both FATs equal
    root = img[(rsvd + nfats * fatsz) * bps:][:root_secs * bps]
    data0 = (rsvd + nfats * fatsz + root_secs) * bps
    files = {}
    for e in range(0, len(root), 32):
        ent = root[e:e + 32]
        if ent[0] == 0:
            break
        if ent[11] & 0x08:
            continue
        name = ent[0:8].decode().rstrip() + '.' + ent[8:11].decode().rstrip()
        c, size = struct.unpack_from('<H', ent, 26)[0], struct.unpack_from('<I', ent, 28)[0]
        out = bytearray()
        while 2 <= c < 0xFFF8:
            off = data0 + (c - 2) * spc * bps
            out += img[off:off + spc * bps]
            c = struct.unpack_from('<H', fat, 2 * c)[0]
        files[name.rstrip('.')] = bytes(out[:size])
    return files, clusters, len(img) == tot * bps


class Fat16Image(unittest.TestCase):
    FILES = [('T01.MP3', bytes(range(256)) * 9), ('T02.MP3', b'x' * 1024), ('T03.MP3', b'\x01' * 1025),
             ('INDEX.BIN', b'TIDX' + b'\0' * 100)]

    def test_files_read_back_and_fatfs_sees_fat16(self):
        clusters, spc = st.sd_layout([len(c) for _, c in self.FILES])
        img = st.fat16_image(self.FILES, clusters, spc)
        files, seen, size_ok = read_fat16(img)
        self.assertEqual(files, dict(self.FILES))
        self.assertEqual(seen, clusters)
        self.assertTrue(size_ok)
        self.assertGreater(seen, st.FAT12_MAX_CLUSTERS)

    def test_bigger_clusters_and_total_over_64k_sectors(self):
        files = [('T01.MP3', b'a' * 3_000_000), ('INDEX.BIN', b'i' * 5000)]
        img = st.fat16_image(files, 30_000, 4)           # 30000 x 2 KB = 61 MB, more than 65535 sectors
        got, seen, size_ok = read_fat16(img)
        self.assertEqual(got, dict(files))
        self.assertEqual(seen, 30_000)
        self.assertTrue(size_ok)

    def test_refuses_what_does_not_fit_or_is_not_8_3(self):
        with self.assertRaises(st.SyncError):
            st.fat16_image([('T01.MP3', b'a' * 3_000_000)], 4400, 1)   # 2.2 MB volume
        with self.assertRaises(ValueError):
            st.fat16_image([('a long name.mp3', b'a')], 4400, 1)

    @unittest.skipUnless(os.environ.get('IDF_PATH'), 'needs the ESP-IDF env (. tools/idf_env.sh)')
    def test_idf_fatfsparse_reads_it(self):
        clusters, spc = st.sd_layout([len(c) for _, c in self.FILES])
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / 'card.img'
            img.write_bytes(st.fat16_image(self.FILES, clusters, spc))
            parser = Path(os.environ['IDF_PATH']) / 'components' / 'fatfs' / 'fatfsparse.py'
            subprocess.run([sys.executable, str(parser), str(img)], cwd=d, check=True, capture_output=True)
            for name, content in self.FILES:
                # fatfsparse names its output folder after the volume label
                self.assertEqual((Path(d) / 'TUNES' / name).read_bytes(), content, name)


class PlayStarts(unittest.TestCase):
    """Later plays found by tempo (propose_play_starts) on synthetic onset trains: no ffmpeg needed."""
    HOP = st.ONSET_HOP_S

    def train(self, parts, lead_s=3.0):
        """Onset strength: silence for lead_s, then (seconds, beat period) parts of clicks with an off-beat accent."""
        o = [0.0] * int(lead_s / self.HOP)
        for secs, period in parts:
            n = int(secs / self.HOP)
            seg = [0.0] * n
            t = 0.0
            while t < secs:
                i = int(t / self.HOP)
                if i < n:
                    seg[i] = 1.0
                j = int((t + period * 0.66) / self.HOP)
                if j < n:
                    seg[j] = 0.5
                t += period
            o += seg
        return o

    def test_three_plays_each_faster(self):
        o = self.train([(60, 0.60), (40, 0.42), (30, 0.30)])
        starts = st.propose_play_starts(o, 3000)
        self.assertEqual(len(starts), 3)
        self.assertEqual(starts[0], 3000)
        for got, change in zip(starts[1:], (63.0, 103.0)):
            self.assertLess(abs(got / 1000 - (change - st.PLAY_LEAD_S)), 2.5, starts)

    def test_two_plays(self):
        starts = st.propose_play_starts(self.train([(40, 0.55), (30, 0.38)]), 3000)
        self.assertEqual(len(starts), 2, starts)
        self.assertLess(abs(starts[1] / 1000 - (43.0 - st.PLAY_LEAD_S)), 2.5, starts)

    def test_one_steady_tempo_is_one_play(self):
        self.assertEqual(st.propose_play_starts(self.train([(70, 0.5)]), 3000), [3000])

    def test_too_short_for_a_second_play(self):
        self.assertEqual(st.propose_play_starts(self.train([(20, 0.5)]), 3000), [3000])


class Mark(unittest.TestCase):
    def test_tap_time_takes_off_the_delay_and_rounds(self):
        self.assertEqual(mark.tap_to_ms(80000, 15.0, delay_s=0.3), 94700)
        self.assertEqual(mark.tap_to_ms(0, 0.1, delay_s=0.35), 0)

    def test_fits_between_neighbours(self):
        self.assertTrue(mark.fits(90000, 8000, 140000, 200000))
        self.assertFalse(mark.fits(10000, 8000, 140000, 200000))     # too close to play 1
        self.assertFalse(mark.fits(137000, 8000, 140000, 200000))    # too close to the next
        self.assertFalse(mark.fits(200000, 8000, None, 200000))      # past the end
        self.assertTrue(mark.fits(5000, None, None, 200000))


class BoardLink(unittest.TestCase):
    # Same bytes as GOLDEN in tests/host/test_link.c (the firmware's encoder).
    GOLDEN = bytes.fromhex('4b4c691400000001000000000000000200000000000f0010000000e2793255')

    def test_encode_matches_the_firmware(self):
        self.assertEqual(bl.encode('i', struct.pack('<5I', 1, 0, 2, 983040, 16)), self.GOLDEN)

    def test_decoder_skips_log_text_and_bad_frames(self):
        bad = bytearray(self.GOLDEN)
        bad[10] ^= 1
        stream = b'I (786) app: KL volume 8\r\n' + bytes(bad) + self.GOLDEN + b'W (9) KLx' + bl.encode('w', b'abc')
        for step in (1, 5, 4096):
            d, got = bl.Decoder(), []
            for i in range(0, len(stream), step):
                got += d.feed(stream[i:i + step])
            self.assertEqual(got, [('i', self.GOLDEN[7:27]), ('w', b'abc')])

    def test_write_order_ends_with_sector_0_and_covers_every_sector_once(self):
        for n in (1, 2, 64, 65, 129, 1000):
            pieces = bl.write_order(n)
            self.assertEqual(pieces[-1], (0, 1))
            covered = sorted(s for first, count in pieces for s in range(first, first + count))
            self.assertEqual(covered, list(range(n)))
            self.assertTrue(all(0 < c <= bl.MAX_SECTORS for _, c in pieces))


if __name__ == '__main__':
    unittest.main()
