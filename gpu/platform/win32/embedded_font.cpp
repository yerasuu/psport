// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the overlay's DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license)
// in a COFF object, which has neither .hidden nor .previous.
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global psport_font_ttf\n"
    "psport_font_ttf:\n"
    ".incbin \"" PS_FONT_PATH "\"\n"
    ".global psport_font_ttf_end\n"
    "psport_font_ttf_end:\n"
    ".text\n");
