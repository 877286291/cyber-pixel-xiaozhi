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
# banner, and mock subtitles are English. There is deliberately no CJK coverage
# in the pixel fonts; the AI subtitles are Chinese, so a separate subsetted
# font (cyber_font_cjk, see the CJK section at the end of this script) renders
# them, per docs/development/engineering/coding-conventions.md.
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

# ---------------------------------------------------------------------------
# CJK subtitle font (cyber_font_cjk)
#
# The AI subtitles are server-driven Chinese, which the ASCII-only pixel fonts
# cannot render. This is a *separate* font from the pixel set, generated from
# the Noto Sans SC source (SIL OFL 1.1, see noto-sans-sc/OFL.txt).
#
# Range: printable ASCII, the basic CJK block 0x4E00-0x9FFF (about 6.7k
# characters, the GB2312 core set) and the Chinese punctuation codepoints that
# assistant replies and STT transcripts actually use:
#   0x3000-0x303F  CJK punctuation  、。「」『』【】〔〕（…）etc.
#   0xFF00-0xFFEF fullwidth forms    ，！？：；＠＃＄％＆＇（）＊＋…
#   0x2014         em dash            ——
#   0x2018-0x201F  curly quotes       “”‘’
#   0x2026         horizontal ellipsis …
# Subtitles are server-driven dynamic text, so punctuation is part of the
# supported charset even though the static UI copy itself is ASCII only.
# Characters outside this range still fall back to the LVGL placeholder box
# (CONFIG_LV_USE_FONT_PLACEHOLDER=y), which is deliberate rather than silent
# blank. The extra ~140 punctuation codepoints add only tens of KB at 1 bpp.
#
# 1 bpp (no antialiasing) is intentional: at 16 px a 4 bpp version of this
# range is about 10 MB of C text, which would not fit the 8 MB flash, while the
# 1 bpp version links to a few MB and keeps the firmware at roughly 70% free.
# 16 px CJK at 1 bpp is fully legible on this 240x320 display and matches the
# non-antialiased pixel aesthetic. --no-compress is used so the font does not
# depend on the compressed-glyph decoder configuration.
#
# This step needs the Noto Sans SC TTF in this directory; the committed
# generated/cyber_font_cjk.c is what the firmware actually compiles.
gen_cjk() {
    local out="$1"
    printf 'generating %s  (Noto Sans SC, 16 px, 1 bpp)\n' "$out"
    npx --yes "$CONVERTER" \
        --font "noto-sans-sc/NotoSansSC-VF.ttf" \
        --range "0x20-0x7E,0x4E00-0x9FFF,0x3000-0x303F,0xFF00-0xFFEF,0x2014,0x2018-0x201F,0x2026" \
        --size 16 \
        --bpp 1 \
        --format lvgl \
        --no-compress \
        --lv-font-name "cyber_font_cjk" \
        --lv-include lvgl.h \
        -o "generated/$out"
}
gen_cjk cyber_font_cjk.c

printf 'done: %s\n' "$(ls generated)"
