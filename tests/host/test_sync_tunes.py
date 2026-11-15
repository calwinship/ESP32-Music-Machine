"""Host tests for tools/sync_tunes.py (stdlib only; run by ./test). No ffmpeg, IDF or board needed."""
import struct
import sys
import tempfile
import unittest
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'tools'))
import sync_tunes as st  # noqa: E402

FIXTURE = HERE / 'fixtures' / 'index_v1.bin'


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
        text = st.marker_file_text('Clare Jig', [6800], '2026-11-15')
        self.assertEqual(st.parse_markers(text, 200000, 'f'), [6800])

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
        #   python3 tools/sync_tunes.py --write-test-fixture tests/host/fixtures/index_v1.bin
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


if __name__ == '__main__':
    unittest.main()
