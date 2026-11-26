"""USB link to the board's firmware (main/link_task.c), used by sync_tunes.py to write the SD card.

Frames and messages: firmware/s3/components/logic/include/logic/link.h. The board's output mixes log text with
frames, so replies are found by their magic and CRC and everything else is skipped.
"""
import struct
import sys
import time
import zlib
from dataclasses import dataclass

MAGIC = b'KL'
HEADER = struct.Struct('<2sBI')
VERSION = 1
SECTOR = 512
MAX_SECTORS = 64
MODE_PLAYER, MODE_SYNC = 0, 1
SOURCES = {0: 'nothing', 1: 'built-in flash', 2: 'SD card'}


class LinkError(Exception):
    pass


def encode(ftype, payload=b''):
    body = struct.pack('<BI', ord(ftype), len(payload)) + payload
    return MAGIC + body + struct.pack('<I', zlib.crc32(body))


class Decoder:
    """Finds frames in a byte stream with text around them (mirrors logic/link.c)."""

    def __init__(self, max_payload=1024):
        self.buf = bytearray()
        self.max_payload = max_payload

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                del self.buf[:max(0, len(self.buf) - 1)]   # keep a trailing 'K'
                return out
            del self.buf[:i]
            if len(self.buf) < HEADER.size:
                return out
            _, ftype, n = HEADER.unpack_from(self.buf)
            if n > self.max_payload:
                del self.buf[:1]
                continue
            if len(self.buf) < HEADER.size + n + 4:
                return out
            body = bytes(self.buf[2:HEADER.size + n])
            (crc,) = struct.unpack_from('<I', self.buf, HEADER.size + n)
            if zlib.crc32(body) == crc:
                out.append((chr(ftype), body[5:]))
                del self.buf[:HEADER.size + n + 4]
            else:
                del self.buf[:1]


@dataclass
class Info:
    version: int
    mode: int
    source: int
    card_sectors: int
    tune_count: int


class Link:
    def __init__(self, monitor_mod, port_timeout=20.0):
        self.mon = monitor_mod
        self.port_timeout = port_timeout
        self.s = None
        self.dec = Decoder()

    def open(self):
        end = time.time() + self.port_timeout
        while True:
            p = self.mon.find_port()
            if p:
                try:
                    self.s = self.mon.open_quiet(p.device, 115200)
                    self.s.timeout = 0.05
                    self.dec = Decoder()
                    return p.device
                except Exception:   # port still settling after a restart
                    pass
            if time.time() > end:
                raise LinkError('no ESP32-S3 port (303A:1001). Plugged in? If the plug is flipped you get the other '
                                'chip: turn it over')
            time.sleep(0.3)

    def close(self):
        if self.s:
            try:
                self.s.close()
            except Exception:
                pass
            self.s = None

    def request(self, ftype, payload, want, timeout=3.0, tries=3):
        frame = encode(ftype, payload)
        for _ in range(tries):
            self.s.write(frame)
            end = time.time() + timeout
            while time.time() < end:
                for rtype, body in self.dec.feed(self.s.read(4096)):
                    if rtype == want:
                        return body
        raise LinkError(f"no '{want}' reply to '{ftype}'")

    def info(self):
        body = self.request('I', b'', 'i', timeout=1.5, tries=4)
        return Info(*struct.unpack('<5I', body[:20]))

    def restart_into(self, ftype, want, mode):
        """Ask the board to restart ('B' sync mode, 'X' player), then reconnect and wait for that mode."""
        self.request(ftype, b'', want)
        self.close()
        time.sleep(1.0)
        end = time.time() + self.port_timeout
        while time.time() < end:
            try:
                self.open()
                inf = self.info()
                if inf.mode == mode:
                    return inf
            except (LinkError, OSError):
                pass
            self.close()
            time.sleep(0.5)
        raise LinkError('the board did not come back after restarting')

    def start(self, total_sectors):
        (err,) = struct.unpack('<i', self.request('S', struct.pack('<I', total_sectors), 's', timeout=5.0))
        if err:
            raise LinkError(f'board could not prepare the card (error 0x{err & 0xffffffff:x})')

    def write(self, first, data):
        count = len(data) // SECTOR
        assert len(data) == count * SECTOR and 0 < count <= MAX_SECTORS
        body = self.request('W', struct.pack('<I', first) + data, 'w', timeout=5.0)
        f, c, err = struct.unpack('<IIi', body[:12])
        if (f, c) != (first, count):
            raise LinkError(f'write reply for sectors {f}+{c}, expected {first}+{count}')
        if err:
            raise LinkError(f'card write failed at sector {first} (error 0x{err & 0xffffffff:x})')


def write_order(n_sectors, chunk=MAX_SECTORS):
    """(first, count) pieces covering sectors 1..n-1, then sector 0 last: the card holds no valid volume until the
    whole image is written (the board clears sector 0 on 'S')."""
    pieces = [(s, min(chunk, n_sectors - s)) for s in range(1, n_sectors, chunk)]
    return pieces + [(0, 1)]


def write_image(link, image, progress=None):
    if len(image) % SECTOR:
        raise LinkError('image is not a whole number of sectors')
    n = len(image) // SECTOR
    link.start(n)
    done, t0 = 0, time.time()
    for first, count in write_order(n):
        link.write(first, image[first * SECTOR:(first + count) * SECTOR])
        done += count
        if progress:
            progress(done, n, time.time() - t0)
    return time.time() - t0


if __name__ == '__main__':   # ./tools/board_link.py: print what the board reports
    sys.path.insert(0, str(__import__('pathlib').Path(__file__).parent))
    import monitor
    link = Link(monitor)
    print('port', link.open())
    print(link.info())
