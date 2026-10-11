// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "font.h"
#include "font_internal.h"
#include "fontft.h"
#include "fontft_internal.h"
#include "font_export.h"
#include "core/libraries/libs.h"

namespace Libraries::FontFt {

s32 PS4_SYSV_ABI sceFontFtInitAliases() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSetAliasFont() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSetAliasPath() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportBdf() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportCid() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportFontFormats() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportOpenType() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportOpenTypeOtf() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportOpenTypeTtf() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportPcf() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportPfr() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportSystemFonts() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportTrueType() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportTrueTypeGx() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportType1() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportType42() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtSupportWinFonts() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontFtTermAliases() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFontSelectGlyphsFt() {
    LOG_ERROR(Lib_FontFt, "(STUBBED) called");
    return ORBIS_OK;
}

const OrbisFontLibrarySelection* PS4_SYSV_ABI sceFontSelectLibraryFt(int value) {
    LOG_INFO(Lib_FontFt, "called");
    LOG_DEBUG(Lib_FontFt, "params:\nvalue: {}\n", value);
    if (value == 0) {
        return Libraries::FontFt::Internal::GetDriverTable();
    }
    return nullptr;
}

const OrbisFontRendererSelection* PS4_SYSV_ABI sceFontSelectRendererFt(int value) {
    LOG_INFO(Lib_FontFt, "called");
    LOG_DEBUG(Lib_FontFt, "params:\nvalue: {}\n", value);
    if (value == 0) {
        return Libraries::FontFt::Internal::GetRendererSelectionTable();
    }
    return nullptr;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    FONT_FUNCTION("e60aorDdpB8", "libSceFontFt", 1, "libSceFontFt", sceFontFtInitAliases);
    FONT_FUNCTION("BxcmiMc3UaA", "libSceFontFt", 1, "libSceFontFt", sceFontFtSetAliasFont);
    FONT_FUNCTION("MEWjebIzDEI", "libSceFontFt", 1, "libSceFontFt", sceFontFtSetAliasPath);
    FONT_FUNCTION("ZcQL0iSjvFw", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportBdf);
    FONT_FUNCTION("LADHEyFTxRQ", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportCid);
    FONT_FUNCTION("+jqQjsancTs", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportFontFormats);
    FONT_FUNCTION("oakL15-mBtc", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportOpenType);
    FONT_FUNCTION("dcQeaDr8UJc", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportOpenTypeOtf);
    FONT_FUNCTION("2KXS-HkZT3c", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportOpenTypeTtf);
    FONT_FUNCTION("H0mJnhKwV-s", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportPcf);
    FONT_FUNCTION("S2mw3sYplAI", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportPfr);
    FONT_FUNCTION("+ehNXJPUyhk", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportSystemFonts);
    FONT_FUNCTION("4BAhDLdrzUI", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportTrueType);
    FONT_FUNCTION("Utlzbdf+g9o", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportTrueTypeGx);
    FONT_FUNCTION("nAfQ6qaL1fU", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportType1);
    FONT_FUNCTION("X9+pzrGtBus", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportType42);
    FONT_FUNCTION("w0hI3xsK-hc", "libSceFontFt", 1, "libSceFontFt", sceFontFtSupportWinFonts);
    FONT_FUNCTION("w5sfH9r8ZJ4", "libSceFontFt", 1, "libSceFontFt", sceFontFtTermAliases);
    FONT_FUNCTION("ojW+VKl4Ehs", "libSceFontFt", 1, "libSceFontFt", sceFontSelectGlyphsFt);
    FONT_FUNCTION("oM+XCzVG3oM", "libSceFontFt", 1, "libSceFontFt", sceFontSelectLibraryFt);
    FONT_FUNCTION("Xx974EW-QFY", "libSceFontFt", 1, "libSceFontFt", sceFontSelectRendererFt);
};

} // namespace Libraries::FontFt
