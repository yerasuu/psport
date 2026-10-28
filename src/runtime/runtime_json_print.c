// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include "runtime_json_print.h"

void runtime_json_print_string(const char *sz_text) {
    putchar('"');
    for (const unsigned char *p_cursor = (const unsigned char *)sz_text; *p_cursor; ++p_cursor) {
        if (*p_cursor == '"' || *p_cursor == '\\') printf("\\%c", *p_cursor);
        else if (*p_cursor == '\n') fputs("\\n", stdout);
        else if (*p_cursor < 0x20) printf("\\u%04x", *p_cursor);
        else putchar(*p_cursor);
    }
    putchar('"');
}
