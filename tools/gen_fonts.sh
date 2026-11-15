#!/bin/zsh
# Generate the Latin-1 UI fonts into firmware/s3/main/fonts/ (generated C, commit them). Usage: tools/gen_fonts.sh
# With them the screen shows Irish fadas (á é í ó ú Á É Í Ó Ú); without them the build uses LVGL's built-in ASCII
# fonts and folds accents (firmware/s3/main/ui_fonts.h).
# Sources: Montserrat-Medium.ttf (SIL Open Font License) and FontAwesome 5 (play/pause symbols), both shipped in
# LVGL 8.4.0's scripts/built_in_font/ (present after one ./build). Converter: lv_font_conv, pinned, fetched by npx
# from the npm registry on first use.
set -euo pipefail
ROOT="${0:A:h:h}"
SRC="$ROOT/firmware/s3/managed_components/lvgl__lvgl/scripts/built_in_font"
OUT="$ROOT/firmware/s3/main/fonts"
CONV=(npx -y lv_font_conv@1.5.2)
[[ -f "$SRC/Montserrat-Medium.ttf" ]] || { echo "gen_fonts: run ./build first (needs $SRC)" >&2; exit 1; }
mkdir -p "$OUT"
# The font symbol is named after the output file (font_text_20.c -> font_text_20).
common=(--bpp 4 --format lvgl --no-compress --no-prefilter --force-fast-kern-format --lv-include lvgl.h)
latin1=0x20-0x7E,0xA0-0xFF

# Titles, list, labels; plus FontAwesome list/play/pause (U+F00B, U+F04B, U+F04C = LV_SYMBOL_LIST/PLAY/PAUSE).
"${CONV[@]}" "${common[@]}" --size 20 --font "$SRC/Montserrat-Medium.ttf" -r "$latin1" \
  --font "$SRC/FontAwesome5-Solid+Brands+Regular.woff" -r 0xF00B,0xF04B,0xF04C -o "$OUT/font_text_20.c"
"${CONV[@]}" "${common[@]}" --size 14 --font "$SRC/Montserrat-Medium.ttf" -r "$latin1" \
  -o "$OUT/font_text_14.c"
# Elapsed time: digits and colon only.
"${CONV[@]}" "${common[@]}" --size 64 --font "$SRC/Montserrat-Medium.ttf" --symbols "0123456789:" \
  -o "$OUT/font_time_64.c"

# Tune icons (tools/icons.txt), 56 px: shown above the title in the tune's colour.
icons=$(grep -v '^#' "$ROOT/tools/icons.txt" | awk 'NF==2 {printf "%s0x%s", sep, $2; sep=","}')
"${CONV[@]}" "${common[@]}" --size 56 --font "$SRC/FontAwesome5-Solid+Brands+Regular.woff" -r "$icons" \
  -o "$OUT/font_icon_56.c"

# Make the next ./build re-run CMake so main/CMakeLists.txt picks the new files up.
touch "$ROOT/firmware/s3/main/CMakeLists.txt"
ls -l "$OUT"
echo "gen_fonts: done; now ./flash"
