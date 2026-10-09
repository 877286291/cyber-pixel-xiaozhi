<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Cyber-pixel UI font set

The application's UI text is rendered from two SIL Open Font License 1.1 families. Both are outline TTFs drawn on an 8 px grid, so rendering at an exact multiple of 8 px keeps glyph edges on the pixel grid and preserves the hard-edged pixel look.

| File | Family, size, bpp | Role | Use |
| --- | --- | --- | --- |
| [`fonts/silkscreen/Silkscreen-Regular.ttf`](fonts/silkscreen/Silkscreen-Regular.ttf) | Silkscreen Regular, outline TTF | Source | Upstream source for the two `cyber_font_*` UI sizes. |
| [`fonts/silkscreen/Silkscreen-Bold.ttf`](fonts/silkscreen/Silkscreen-Bold.ttf) | Silkscreen Bold, outline TTF | Source | Reserved for emphasis; not converted yet. |
| [`fonts/pressstart2p/PressStart2P-Regular.ttf`](fonts/pressstart2p/PressStart2P-Regular.ttf) | Press Start 2P Regular, outline TTF | Source | Upstream source for the three `cyber_font_title*` / `cyber_font_code` sizes. |
| [`fonts/generated/cyber_font_sm.c`](fonts/generated/cyber_font_sm.c) | Silkscreen Regular, 8 px, 4 bpp | Generated | Status-bar and hint labels (`cyber_font_sm`). |
| [`fonts/generated/cyber_font_md.c`](fonts/generated/cyber_font_md.c) | Silkscreen Regular, 16 px, 4 bpp | Generated | State names and AI subtitle text (`cyber_font_md`). |
| [`fonts/generated/cyber_font_title.c`](fonts/generated/cyber_font_title.c) | Press Start 2P Regular, 16 px, 4 bpp | Generated | Headings and short labels (`cyber_font_title`). |
| [`fonts/generated/cyber_font_title_lg.c`](fonts/generated/cyber_font_title_lg.c) | Press Start 2P Regular, 24 px, 4 bpp | Generated | Boot banner and large readouts (`cyber_font_title_lg`). |
| [`fonts/generated/cyber_font_code.c`](fonts/generated/cyber_font_code.c) | Press Start 2P Regular, 48 px, 4 bpp | Generated | The pairing code (`cyber_font_code`); the largest text in the UI, split over two rows by the activation screen. |

- **License.** SIL Open Font License 1.1 for both families; the license text is stored beside each source as `OFL.txt`. Redistribution and embedding are permitted.
- **Converter.** `lv_font_conv` 1.5.3, producing output compatible with the LVGL 9.5.0 pinned in [`../dependencies.lock`](../dependencies.lock). Regenerate everything with `./assets/fonts/generate-fonts.sh`.
- **Integration.** The generated `.c` files are compiled by the `main` component and declared there with `LV_FONT_DECLARE`; copying a font into this directory does not by itself link or select it.
- **Coverage.** The four general-purpose fonts cover printable ASCII only (`0x20-0x7E`). The UI is English-only, so there is deliberately no Chinese glyph coverage; adding Chinese text requires a separate subsetted font plus an explicit unsupported-character policy, per [`../docs/development/engineering/coding-conventions.md`](../docs/development/engineering/coding-conventions.md).
- **`cyber_font_code` is not a general-purpose font.** The pairing code is the one string a user has to read off the screen from arm's length and retype on another device, so it is rendered much larger than any other text. At 48 px, converting the full printable-ASCII range would produce roughly 140 KB of C text to draw ten digits, so the source range is limited to space, `-`, `:` and `0-9`. Any other character renders as nothing at all, silently. Use a different font for prose.
- **Budget.** The five generated sources total about 119 KB of C text; the linked glyph bitmaps are smaller and are not a meaningful share of the 8 MB Flash. The font descriptors are `const` and stay in Flash, so the only RAM cost is LVGL's per-glyph draw buffer.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
