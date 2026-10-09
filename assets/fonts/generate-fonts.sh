#!/usr/bin/env bash
# Regenerate the LVGL font sources used by the cyber-pixel UI.
#
# Both families are licensed under the SIL Open Font License 1.1, which permits
# redistribution and embedding (see silkscreen/OFL.txt and pressstart2p/OFL.txt).
# They are outline TTFs designed on an 8 px grid, so rendering at an exact
# multiple of 8 keeps the glyph edges on the pixel grid and preserves the
# hard-edged pixel look.
#
# Coverage is printable ASCII only (0x20-0x7E): the application UI, the boot
# banner, and mock subtitles are English. There is deliberately no CJK coverage;
# adding Chinese text requires a separate subsetted font plus an unsupported
# character policy, per docs/development/engineering/coding-conventions.md.
#
# 4 bpp keeps antialiasing available for the few glyphs that do not land
# exactly on the grid. The whole set is well under 100 KB of Flash.
#
# Requirements: Node.js (npx). The converter version is pinned so a regenerated
# font stays byte-comparable with the committed sources.
#
#   ./assets/fonts/generate-fonts.sh
set -euo pipefail

cd "$(dirname "$0")"

CONVERTER="lv_font_conv@1.5.3"
RANGE="0x20-0x7E"
BPP=4

mkdir -p generated

# gen <output-file> <source-ttf> <size-px> <c-symbol> [range]
gen() {
    local out="$1" src="$2" size="$3" symbol="$4" range="${5:-$RANGE}"
    printf 'generating %s  (%s, %s px)\n' "$out" "$src" "$size"
    npx --yes "$CONVERTER" \
        --font "$src" \
        --range "$range" \
        --size "$size" \
        --bpp "$BPP" \
        --format lvgl \
        --lv-font-name "$symbol" \
        --no-kerning \
        -o "generated/$out"
}

# Compact status-bar / hint text.
gen cyber_font_sm.c        silkscreen/Silkscreen-Regular.ttf     8  cyber_font_sm
# Body text: state names and AI subtitles.
gen cyber_font_md.c        silkscreen/Silkscreen-Regular.ttf    16  cyber_font_md
# Arcade-style headings and short labels.
gen cyber_font_title.c     pressstart2p/PressStart2P-Regular.ttf 16  cyber_font_title
# Boot banner and the large numeric readouts.
gen cyber_font_title_lg.c  pressstart2p/PressStart2P-Regular.ttf 24  cyber_font_title_lg
# Pairing-code readout: the one piece of text a user has to read off the screen
# from arm's length and retype elsewhere, so it is rendered far larger than any
# other text and split over two rows by the UI.
#
# Deliberately not the full printable-ASCII range. At 48 px, printable ASCII
# would be roughly 140 KB of C text, while this font only ever renders 0-9 plus
# the '-' placeholder and the ':' separator. Coverage is therefore space, '-',
# ':' and the digits. Do not use it for prose: any other character renders as
# nothing at all, silently.
gen cyber_font_code.c      pressstart2p/PressStart2P-Regular.ttf 48  cyber_font_code "0x20,0x2D,0x30-0x3A"

printf 'done: %s\n' "$(ls generated)"
