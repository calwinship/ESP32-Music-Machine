#!/usr/bin/env python3
"""Non-interactive serial log capture for the ESP32-S3 (replacement for `idf.py monitor`, which needs a TTY).

Resets the board, prints N seconds of log output, then exits.

The S3's native USB-Serial-JTAG port (Espressif VID 0x303A) disappears from the host when the chip resets and
re-enumerates a moment later, possibly under a different /dev name. This script survives that: on a read error or
a vanished port it waits for a port with the same USB serial number (or, failing that, the same VID:PID) to reappear,
reopens it and keeps capturing until the deadline.

Exit codes: 0 = captured some output, 1 = bad arguments, 2 = no port found, 3 = port found but no output.
"""
from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial missing: run this with the ESP-IDF Python (source export.sh first) or `pip install pyserial`.")

ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001


def find_port(want_serial: str | None = None, want_device: str | None = None):
    """Return the best-matching ListPortInfo, or None.

    Preference: explicit device path > same USB serial number > any Espressif USB-Serial-JTAG port.
    """
    ports = list(list_ports.comports())
    if want_device:
        return next((p for p in ports if p.device == want_device), None)
    if want_serial:
        match = next((p for p in ports if p.serial_number == want_serial), None)
        if match:
            return match
    return next((p for p in ports if p.vid == ESPRESSIF_VID and p.pid == USB_SERIAL_JTAG_PID), None)


def set_rts(s: serial.Serial, state: bool) -> None:
    # Re-write DTR after RTS so the control-line request is actually sent (same workaround as esptool /
    # esp-idf-monitor; harmless on macOS/Linux).
    s.rts = state
    s.dtr = s.dtr


def open_quiet(device: str, baud: int) -> serial.Serial:
    """Open the port without resetting the chip.

    macOS asserts DTR and RTS when a tty is opened. USB-Serial-JTAG resets the chip when it sees RTS asserted
    with DTR de-asserted, so the lines must be released RTS first, then DTR, never the other way round.
    This mirrors esp_idf_monitor's open_serial(). (Verified on hardware 2026-11-14: releasing DTR first reset
    the S3 on every open.)
    """
    s = serial.Serial()
    s.port = device
    s.baudrate = baud
    s.timeout = 0.1
    s.dtr = True   # match the state the OS gives the port on open: no transition, no reset
    s.rts = True
    s.open()
    set_rts(s, False)
    s.dtr = False
    return s


def reset_chip(s: serial.Serial, mode: str) -> None:
    """Hard-reset into the application (not download mode).

    usbjtag: USB-Serial-JTAG maps RTS to chip reset and DTR to the boot strap; DTR must stay low.
    classic: USB-UART bridge with the usual two-transistor auto-reset circuit (RTS -> EN, DTR -> GPIO0).
    """
    if mode == "none":
        return
    s.dtr = False
    set_rts(s, True)
    time.sleep(0.2 if mode == "usbjtag" else 0.1)
    set_rts(s, False)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seconds", nargs="?", type=float, default=10.0, help="capture duration after reset (default 10)")
    ap.add_argument("-p", "--port", help="serial device (default: auto-detect Espressif USB-Serial-JTAG)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (ignored by USB-Serial-JTAG)")
    ap.add_argument("--reset", choices=["auto", "usbjtag", "classic", "none"], default="auto",
                    help="how to reset before capturing (default auto: usbjtag for VID 0x303A, else classic)")
    ap.add_argument("--wait-port", type=float, default=5.0, help="seconds to wait for the port to appear (default 5)")
    ap.add_argument("-o", "--out", help="also append the raw capture to this file")
    ap.add_argument("--allow-non-espressif", action="store_true",
                    help="allow opening a non-Espressif port (on this board: the ESP32-U4WDH bridge; ask first)")
    args = ap.parse_args(argv)
    if args.seconds <= 0:
        ap.error("seconds must be > 0")

    def note(msg: str) -> None:
        print(f"[monitor] {msg}", file=sys.stderr, flush=True)

    deadline_find = time.monotonic() + args.wait_port
    info = None
    while info is None and time.monotonic() < deadline_find:
        info = find_port(want_device=args.port)
        if info is None:
            time.sleep(0.2)
    if info is None:
        note("no port found" + (f" at {args.port}" if args.port else " (no Espressif USB-Serial-JTAG device)")
             + ". Is the board plugged in? Try flipping the USB-C plug.")
        listed = ", ".join(f"{p.device} [{p.vid and hex(p.vid)}:{p.pid and hex(p.pid)}]" for p in list_ports.comports())
        note(f"ports present: {listed or 'none'}")
        return 2

    want_serial = info.serial_number
    is_jtag = info.vid == ESPRESSIF_VID
    if not is_jtag and not args.allow_non_espressif:
        # On this board a non-Espressif port is the ESP32-U4WDH's CH340 bridge (plug flipped). Opening it can pulse the
        # ESP32's EN/IO0 through the auto-reset circuit, and that chip must not be touched without asking.
        note(f"{info.device} is not the S3 (vid:pid={info.vid and hex(info.vid)}:{info.pid and hex(info.pid)}); "
             "refusing to open it. Flip the USB-C plug, or pass --allow-non-espressif.")
        return 2
    mode = args.reset if args.reset != "auto" else ("usbjtag" if is_jtag else "classic")
    note(f"port {info.device} vid:pid={info.vid and hex(info.vid)}:{info.pid and hex(info.pid)} "
         f"serial={want_serial} reset={mode}")

    out_file = open(args.out, "ab") if args.out else None
    got_bytes = 0
    reconnects = 0
    s = open_quiet(info.device, args.baud)
    reset_chip(s, mode)
    t0 = time.monotonic()
    end = t0 + args.seconds
    try:
        while time.monotonic() < end:
            try:
                if s is None:
                    raise serial.SerialException("not open")
                data = s.read(4096)
            except (serial.SerialException, OSError):
                # Port vanished (chip reset re-enumerates USB). Wait for it to come back.
                if s is not None:
                    try:
                        s.close()
                    except Exception:
                        pass
                    s = None
                    note(f"port lost at +{time.monotonic() - t0:.2f}s, waiting for it to return")
                info = find_port(want_serial=want_serial)
                if info is None:
                    time.sleep(0.05)
                    continue
                try:
                    s = open_quiet(info.device, args.baud)
                    reconnects += 1
                    note(f"reconnected {info.device} at +{time.monotonic() - t0:.2f}s")
                except (serial.SerialException, OSError):
                    s = None
                    time.sleep(0.05)
                continue
            if data:
                got_bytes += len(data)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if out_file:
                    out_file.write(data)
    except KeyboardInterrupt:
        pass
    finally:
        if s is not None:
            s.close()
        if out_file:
            out_file.close()
    note(f"done: {got_bytes} bytes in {args.seconds:g}s, {reconnects} reconnect(s)")
    return 0 if got_bytes else 3


if __name__ == "__main__":
    sys.exit(main())
