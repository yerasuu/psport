// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2Report*: up to eight handlers per report type, the handle being the address of the handler's
// slot (spec 08 §3). Only type 0, the error message of every failing call, is emitted so far; the
// API trace (1 to 3), voice control (4) and render (5 to 7) reports come with the render (F3).
#include <algorithm>
#include <array>
#include <cstring>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

struct ReportSlot {
    OrbisNgs2ReportHandler pfn_handler;
    uintptr_t user_data;
};

struct ReportType {
    std::mutex mutex;
    std::atomic<u32> c_handlers;
    std::array<ReportSlot, kReportHandlersPerType> slots;
};

std::array<ReportType, kReportTypes> s_types;

} // namespace

bool HasReportHandlers(u32 type) {
    return s_types[type].c_handlers.load(std::memory_order_relaxed) != 0;
}

void EmitErrorReport(OrbisNgs2Handle h_object, s32 code, const std::string& message) {
    std::array<ReportSlot, kReportHandlersPerType> slots;
    {
        std::scoped_lock lock{s_types[0].mutex};
        slots = s_types[0].slots;
    }
    // The firmware formats into 1024 bytes (vsprintf_s), so a handler never sees more.
    char sz_message[1024];
    const std::size_t cb_message = std::min(message.size(), sizeof(sz_message) - 1);
    std::memcpy(sz_message, message.data(), cb_message);
    sz_message[cb_message] = '\0';
    const OrbisNgs2ReportMessageData data{{sizeof(OrbisNgs2ReportMessageData), h_object, 0, code}, sz_message};
    for (const auto& slot : slots) {
        if (slot.pfn_handler) {
            slot.pfn_handler(&data.header, slot.user_data);
        }
    }
}

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 report_type, OrbisNgs2ReportHandler pfn_handler,
                                              uintptr_t user_data, OrbisNgs2Handle* p_out_handle) {
    if (report_type >= kReportTypes) {
        return ORBIS_NGS2_ERROR_INVALID_REPORT_TYPE;
    }
    if (!pfn_handler) {
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLER;
    }
    OrbisNgs2Handle h_report = 0;
    s32 result = ORBIS_NGS2_ERROR_EMPTY_REPORT_HANDLE;
    {
        auto& type = s_types[report_type];
        std::scoped_lock lock{type.mutex};
        for (auto& slot : type.slots) {
            if (!slot.pfn_handler) {
                slot = {pfn_handler, user_data};
                type.c_handlers.fetch_add(1, std::memory_order_relaxed);
                h_report = reinterpret_cast<OrbisNgs2Handle>(&slot);
                result = ORBIS_OK;
                break;
            }
        }
    }
    // The firmware writes the out handle, 0 when every slot is taken, but does not require one.
    if (p_out_handle) {
        *p_out_handle = h_report;
    }
    if (result == ORBIS_OK && report_type != 0) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2ReportRegisterHandler: report types 1-8 (never emitted)");
    }
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle h_report) {
    if (!h_report) {
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    for (auto& type : s_types) {
        std::scoped_lock lock{type.mutex};
        for (auto& slot : type.slots) {
            if (reinterpret_cast<OrbisNgs2Handle>(&slot) == h_report && slot.pfn_handler) {
                slot = {};
                type.c_handlers.fetch_sub(1, std::memory_order_relaxed);
                return ORBIS_OK;
            }
        }
    }
    return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
}

} // namespace Libraries::Ngs2
