#!/bin/zsh
# Re-create docs/vendor/ (gitignored): download the Waveshare files and datasheets, verify them against
# docs/ref/vendor-files.sha256, and extract each zip into its own folder.
# Usage: tools/fetch_vendor.sh
set -euo pipefail
ROOT="${0:A:h:h}"
V="$ROOT/docs/vendor"
B=https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8
C=https://files.waveshare.com/wiki/common
mkdir -p "$V/datasheets"
cd "$V"
fetch() { [[ -f "$2" ]] || curl -fsSL -A "Mozilla/5.0" -o "$2" "$1"; }
for f in Demo schematic BIN; do fetch "$B/ESP32-S3-Knob-Touch-LCD-1.8-$f.zip" "ESP32-S3-Knob-Touch-LCD-1.8-$f.zip"; done
fetch "$C/Pcm5100a.pdf" datasheets/Pcm5100a.pdf
fetch "$C/Esp32_datasheet_en.pdf" datasheets/Esp32_datasheet_en.pdf
shasum -a 256 -c "$ROOT/docs/ref/vendor-files.sha256"
# bsdtar (macOS tar) copes with the GBK-encoded names that macOS unzip chokes on; it still skips 3 Chinese-named
# third-party datasheets inside SensorLib (expected, and not needed).
for f in Demo schematic BIN; do
  [[ -d "$f" ]] || { mkdir "$f"; tar -xf "ESP32-S3-Knob-Touch-LCD-1.8-$f.zip" -C "$f" 2>/dev/null || true; }
done
echo "docs/vendor ready: $(find Demo -type f | wc -l | tr -d ' ') demo files (expect 1799)"
