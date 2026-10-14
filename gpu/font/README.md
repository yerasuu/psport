# Font

The PS4 `libSceFont` and `libSceFontFt` (font libraries, renderers, font sets, glyph metrics and
rendering over FreeType), from shadPS4 (https://github.com/shadps4-emu/shadPS4, GPL-2.0-or-later)
at commit `3a9f572`, `src/core/libraries/font`. Its structures are the console's PS4 layouts. The
first commit here is the files as they were there; later ones adapt them to libpsport-gpu.

Fonts: `PS_SYS_FONTS=<folder>` points at the user's own dump of the console's system fonts (SST and
the rest, directly in it or under `font`/`font2`). Without them each font set falls back to the
Noto fonts shadPS4 bundles, read here from the host (`PS_FALLBACK_FONTS`, folders separated by `:`,
then the user's and the system's font folders), and the Latin one to the DejaVu Sans libpsport-gpu
embeds for its overlay. The file each font set uses is logged once (`Font: set 0x... -> ...`).
