// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the overlay's DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license)
// in an ELF object: read-only data, hidden in libpsport-gpu.
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden psport_font_ttf\n"
    ".global psport_font_ttf\n"
    "psport_font_ttf:\n"
    ".incbin \"" PS_FONT_PATH "\"\n"
    ".hidden psport_font_ttf_end\n"
    ".global psport_font_ttf_end\n"
    "psport_font_ttf_end:\n"
    ".previous\n");
