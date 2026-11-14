# Flash backups

Backups live **outside the repo** in `~/esp/backups/`. Only their checksums are recorded here.

| File | Chip | Range | SHA-256 | Verified |
|---|---|---|---|---|
| `knob-s3-factory-44bd8d7aa650-2026-11-14.bin` | ESP32-S3 (MAC 44:bd:8d:7a:a6:50) | 0x0–0xFFFFFF (16 MB) | `612016638a25708db24d3828c95c804628817df7ebde83ade150e3e2148b81ea` | `esptool verify_flash` digest matched, 2026-11-14 |

Notes
- Taken before the first flash, using `esptool.py --baud 921600 read_flash 0 0x1000000 <file>` (IDF v5.3.2, esptool 4.13.0).
- The shipped S3 firmware is **not** the vendor's `WX-ESP32S3-KNOB_V1.2.bin`: 510 of that file's 523 4 KB blocks differ from this
  backup. Restore from this backup, not the vendor BIN.
- Restore (S3 only): `esptool.py --port <port> write_flash 0 ~/esp/backups/knob-s3-factory-44bd8d7aa650-2026-11-14.bin`
- No backup of the ESP32-U4WDH has been taken; it has not been touched.
