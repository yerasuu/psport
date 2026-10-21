// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// psport: Bloodborne's summon client (PsNetTitle::SummonClient): the seamless summon broker's
// calls on the community server's /summon_messenger, sent to the shadNet WebAPI with the host's
// placement, cleaned of stale records, repeated for a fast sign and captured, and the
// SHADPS4_BLOODBORNE_SEAMLESS_COOP and SHADPS4_CAPTURE_BLOODBORNE_SUMMON switches. Moved from the
// online module's http.cpp (shadp2p and BBSC), which hands every request to it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/bbsc_persistence.h"
#include "core/bloodborne_re.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/libraries/network/http_plan.h"
#include "psport_net_title.h"

namespace {

using Libraries::Http::HeaderNameMatches;
using Libraries::Http::HostOverrideTarget;
using Libraries::Http::HttpMethodName;
using Libraries::Http::HttpResponse;
using Libraries::Http::ParseHostOverrideTarget;
using Libraries::Http::RunRealHttpRequest;
using Libraries::Http::SendRequestPlan;

bool EnvFlagEnabled(const char* name) {
    return Core::Bbsc::SwitchEnabled(name);
}

// psport: the summon helpers act on the launched title's summon route only (its Game::Title, through
// PsNetHost), so that another game's requests are never taken for Bloodborne's. For a title that
// declares none, either SHADPS4_* switch still forces shadp2p's own route.
constexpr std::string_view InheritedSummonRoute = "/summon_messenger";

std::string SummonPathPrefix(std::string_view title_route, bool is_seamless_on, bool is_capture_on) {
    if (title_route.empty() && !is_seamless_on && !is_capture_on) {
        return {};
    }
    return std::string{title_route.empty() ? InheritedSummonRoute : title_route} + '/';
}

const std::string& SummonPrefix() {
    static const std::string prefix = [] {
        const std::string_view title_route = PsNetTitle::SummonRoute();
        std::string chosen =
            SummonPathPrefix(title_route, EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP"),
                             EnvFlagEnabled("SHADPS4_CAPTURE_BLOODBORNE_SUMMON"));
        if (title_route.empty() && !chosen.empty()) {
            LOG_WARNING(Lib_Http,
                        "Summon route {} forced by SHADPS4_BLOODBORNE_SEAMLESS_COOP or "
                        "SHADPS4_CAPTURE_BLOODBORNE_SUMMON: the title declares none",
                        chosen);
        }
        return chosen;
    }();
    return prefix;
}

bool IsSummonSubPath(std::string_view path, std::string_view sub_path) {
    const std::string& prefix = SummonPrefix();
    return !prefix.empty() && path.find(prefix + std::string{sub_path}) != std::string_view::npos;
}

bool IsBloodborneSummonPath(std::string_view path) {
    return IsSummonSubPath(path, "");
}

constexpr std::string_view BloodborneHostPlacementHeader = "X-ShadPS4-Bloodborne-Host-Placement";

bool IsBloodborneSummonRequestPath(std::string_view path) {
    return IsSummonSubPath(path, "request");
}

bool IsBloodborneSummonSearchPath(std::string_view path) {
    return IsSummonSubPath(path, "get");
}

bool IsBloodborneSummonCreatePath(std::string_view path) {
    return IsSummonSubPath(path, "create");
}

std::optional<std::string_view> FindResponseHeader(std::string_view headers,
                                                   std::string_view name) {
    size_t line_start = 0;
    while (line_start < headers.size()) {
        const size_t line_end = headers.find("\r\n", line_start);
        const std::string_view line = headers.substr(line_start, line_end == std::string_view::npos
                                                                     ? headers.size() - line_start
                                                                     : line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && HeaderNameMatches(line.substr(0, colon), name)) {
            size_t value_start = colon + 1;
            while (value_start < line.size() &&
                   (line[value_start] == ' ' || line[value_start] == '\t')) {
                ++value_start;
            }
            return line.substr(value_start);
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 2;
    }
    return std::nullopt;
}

bool ShouldCaptureBloodborneSummon(const SendRequestPlan& plan) {
    return EnvFlagEnabled("SHADPS4_CAPTURE_BLOODBORNE_SUMMON") && IsBloodborneSummonPath(plan.path);
}

void CaptureBloodborneSummon(const SendRequestPlan& plan, const HttpResponse& response,
                             s32 request_error) {
    if (!ShouldCaptureBloodborneSummon(plan)) {
        return;
    }

    static std::atomic<u64> next_capture_id{1};
    const u64 capture_id = next_capture_id.fetch_add(1, std::memory_order_relaxed);
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto capture_dir =
        Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) / "bloodborne-summon";
    std::error_code error;
    std::filesystem::create_directories(capture_dir, error);
    if (error) {
        LOG_ERROR(Lib_Http, "Could not create Bloodborne summon capture directory {}: {}",
                  Common::FS::PathToUTF8String(capture_dir), error.message());
        return;
    }

    const std::string stem = "summon-" + std::to_string(timestamp) + "-p" +
                             std::to_string(Core::Debugger::GetCurrentPid()) + '-' +
                             std::to_string(capture_id);
    const auto request_path = capture_dir / (stem + "-request.bin");
    const auto response_path = capture_dir / (stem + "-response.bin");
    const auto metadata_path = capture_dir / (stem + ".txt");

    std::ofstream request_file(request_path, std::ios::binary);
    std::ofstream response_file(response_path, std::ios::binary);
    std::ofstream metadata_file(metadata_path);
    if (!request_file || !response_file || !metadata_file) {
        LOG_ERROR(Lib_Http, "Could not open Bloodborne summon capture files under {}",
                  Common::FS::PathToUTF8String(capture_dir));
        return;
    }

    request_file.write(reinterpret_cast<const char*>(plan.body.data()),
                       static_cast<std::streamsize>(plan.body.size()));
    response_file.write(reinterpret_cast<const char*>(response.body.data()),
                        static_cast<std::streamsize>(response.body.size()));
    metadata_file << "method=" << HttpMethodName(plan.method) << '\n'
                  << "url=" << plan.scheme << "://" << plan.host << ':' << plan.port << plan.path
                  << '\n'
                  << "request_bytes=" << plan.body.size() << '\n'
                  << "status=" << response.status_code << '\n'
                  << "response_bytes=" << response.body.size() << '\n'
                  << "request_error=" << request_error << '\n';
    for (const auto& [name, value] : plan.headers) {
        if (HeaderNameMatches(name, BloodborneHostPlacementHeader)) {
            metadata_file << "host_placement_request=" << value << '\n';
        }
    }
    if (const auto placement =
            FindResponseHeader(response.all_headers_blob, BloodborneHostPlacementHeader)) {
        metadata_file << "host_placement_response=" << *placement << '\n';
    }

    LOG_INFO(Lib_Http, "Captured Bloodborne summon exchange {} under {}", capture_id,
             Common::FS::PathToUTF8String(capture_dir));
}

void RewriteHostHeader(SendRequestPlan& plan) {
    std::string host_value = plan.host;
    const bool default_port =
        (plan.scheme == "https" && plan.port == 443) || (plan.scheme == "http" && plan.port == 80);
    if (!default_port) {
        host_value += ":" + std::to_string(plan.port);
    }
    for (auto& [k, v] : plan.headers) {
        if (HeaderNameMatches(k, "Host")) {
            v = host_value;
        }
    }
}

// BBSC connect-first cross-map summon: once the broker has named the host's map (placement
// header), the guest's sign is advertised in that map so the broker matches and delivers the claim
// without the teleport-first step (see bloodborne_re.cpp).
static void RewriteConnectFirstAreaId(SendRequestPlan& plan) {
    if (!IsBloodborneSummonCreatePath(plan.path)) {
        return;
    }
    const auto host_map = Core::Bloodborne::GetBbscConnectFirstAreaId();
    if (!host_map) {
        return;
    }
    constexpr std::string_view Key = "\"AreaId\":";
    const std::string_view body{reinterpret_cast<const char*>(plan.body.data()), plan.body.size()};
    const size_t key = body.find(Key);
    if (key == std::string_view::npos) {
        return;
    }
    const size_t value_begin = key + Key.size();
    size_t value_end = value_begin;
    while (value_end < body.size() && body[value_end] >= '0' && body[value_end] <= '9') {
        ++value_end;
    }
    if (value_end == value_begin) {
        return;
    }
    const std::string original{body.substr(value_begin, value_end - value_begin)};
    const std::string replacement = std::to_string(*host_map);
    if (original == replacement) {
        return;
    }
    plan.body.erase(plan.body.begin() + value_begin, plan.body.begin() + value_end);
    plan.body.insert(plan.body.begin() + value_begin, replacement.begin(), replacement.end());
    std::erase_if(plan.headers,
                  [](const auto& header) { return HeaderNameMatches(header.first, "Content-Length"); });
    LOG_INFO(Lib_Http, "BBSC connect-first: advertisement AreaId {} -> {}", original, replacement);
}

bool ApplyBloodborneSeamlessRoute(SendRequestPlan& plan) {
    if (!EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP") ||
        !EmulatorSettings.IsShadNetEnabled() || !IsBloodborneSummonPath(plan.path)) {
        return false;
    }

    const std::string base_url = EmulatorSettings.GetShadNetWebApiServer();
    const HostOverrideTarget target = ParseHostOverrideTarget(base_url);
    if (target.host.empty()) {
        LOG_WARNING(Lib_Http,
                    "Bloodborne seamless route requested, but shadNet WebAPI server is empty");
        return false;
    }

    const std::string old_scheme = plan.scheme;
    const std::string old_host = plan.host;
    const u16 old_port = plan.port;

    if (!target.scheme.empty()) {
        plan.scheme = target.scheme;
    }
    plan.host = target.host;
    if (target.port != 0) {
        plan.port = target.port;
    } else if (plan.scheme != old_scheme) {
        plan.port = (plan.scheme == "https") ? 443 : 80;
    }
    RewriteHostHeader(plan);
    plan.headers.emplace_back("X-ShadPS4-Bloodborne-Seamless", "1");
    // BBSC: SHADPS4_BBSC_NO_PLACEMENT_HEADER=1 disables the host-placement transport in both
    // directions (isolation test for the seamless join stall; cross-map preparation needs it).
    if (!EnvFlagEnabled("SHADPS4_BBSC_NO_PLACEMENT_HEADER") &&
        (IsBloodborneSummonRequestPath(plan.path) || IsBloodborneSummonSearchPath(plan.path))) {
        if (const auto placement = Core::Bloodborne::GetSeamlessHostPlacementHeader()) {
            plan.headers.emplace_back(BloodborneHostPlacementHeader, *placement);
        }
    }

    // BBSC connect-first cross-map summon: advertise the sign in the host's map so the broker
    // matches and delivers the claim without the teleport-first step (see bloodborne_re.cpp).
    RewriteConnectFirstAreaId(plan);

    LOG_INFO(Lib_Http, "Bloodborne seamless route active: {}://{}:{}{} -> {}://{}:{}{}", old_scheme,
             old_host, old_port, plan.path, plan.scheme, plan.host, plan.port, plan.path);
    return true;
}

// BBSC: the seamless summon broker keeps Preparing/Claimed/Delivered records for 15 minutes and
// ignores ordinary removals. Bloodborne reuses the same SessionId/UserId after an emulator restart,
// so a record left by a crashed or closed session made the next advertisement receive a stale claim
// for a room that no longer exists (observed 2026-09-15 22:09–22:13: host claimed the old record
// before the guest advertised; the guest never joined). Before the first seamless advertisement of
// this process, send a forced removal for the same identity. Once per process only, so in-session
// cross-map preparation (which relies on retained records) is untouched.
std::atomic<bool> g_bbsc_stale_summon_cleared{false};

void ClearStaleSeamlessSummonRecord(const SendRequestPlan& plan) {
    if (!Core::Bbsc::BuiltIn || !Core::Bbsc::Started()) {
        return; // vanilla build: the bbsc.dll mod does its own cleanup
    }
    if (!IsBloodborneSummonCreatePath(plan.path) ||
        std::none_of(plan.headers.begin(), plan.headers.end(), [](const auto& header) {
            return HeaderNameMatches(header.first, "X-ShadPS4-Bloodborne-Seamless");
        }) ||
        g_bbsc_stale_summon_cleared.exchange(true)) {
        return;
    }
    const auto advertisement = nlohmann::json::parse(plan.body.begin(), plan.body.end(), nullptr, false);
    if (!advertisement.is_object()) {
        LOG_WARNING(Lib_Http, "BBSC stale summon cleanup: advertisement body is not a JSON object");
        return;
    }
    std::string session_id;
    s64 user_id = -1;
    if (const auto it = advertisement.find("SessionId"); it != advertisement.end() && it->is_string()) {
        session_id = it->get<std::string>();
    }
    if (const auto it = advertisement.find("UserId"); it != advertisement.end() && it->is_number_integer()) {
        user_id = it->get<s64>();
    }
    if (session_id.empty() || user_id < 0) {
        return;
    }

    SendRequestPlan removal = plan;
    const std::string create_path = SummonPrefix() + "create";
    const auto create_pos = removal.path.find(create_path);
    removal.path.replace(create_pos, create_path.size(), SummonPrefix() + "delete");
    const std::string body = nlohmann::json{{"MessageId", "SummonDataRemoveRequest"},
                                            {"SessionId", session_id},
                                            {"UserId", user_id},
                                            {"Force", true}}
                                 .dump();
    removal.body.assign(body.begin(), body.end());
    std::erase_if(removal.headers, [](const auto& header) {
        return HeaderNameMatches(header.first, "Content-Length") ||
               HeaderNameMatches(header.first, BloodborneHostPlacementHeader);
    });

    HttpResponse response;
    u32 event_bits = 0;
    const s32 error = RunRealHttpRequest(removal, response, event_bits);
    LOG_INFO(Lib_Http,
             "BBSC stale summon cleanup for session {} user {}: error={} status={} body_bytes={}",
             session_id, user_id, error, response.status_code, response.body.size());
}

void Answered(const SendRequestPlan& plan, HttpResponse& local_res, u32& success_event_bits,
              const std::function<bool()>& is_abandoned) {
    const auto absorb_placement = [&plan](const HttpResponse& res) {
        if (!IsBloodborneSummonPath(plan.path) ||
            EnvFlagEnabled("SHADPS4_BBSC_NO_PLACEMENT_HEADER")) {
            return;
        }
        if (const auto placement =
                FindResponseHeader(res.all_headers_blob, BloodborneHostPlacementHeader);
            placement.has_value() &&
            !Core::Bloodborne::SetSeamlessHostPlacementHeader(*placement)) {
            LOG_WARNING(Lib_Http, "Ignored invalid Bloodborne host-placement header");
        } else if (!placement.has_value() && IsBloodborneSummonCreatePath(plan.path)) {
            Core::Bloodborne::ClearSeamlessHostPlacementHeader();
        }
    };
    absorb_placement(local_res);

    // BBSC fast sign: the game re-posts its co-op sign only every ~30 s whatever ss.info says,
    // and a cross-map summon needs three or four of those round trips with the broker
    // (advertised -> preparing with the host's placement -> re-advertised in the host's map ->
    // claimed and delivered): about 2 minutes on 2026-09-23/24 against 5 s on the same map.
    // So the guest's own post is repeated here every 3 s, with the host's map written in as
    // soon as the broker names it, until the broker hands back the claim or 15 s have passed.
    // The game receives the last reply as the answer to the post it made.
    const bool seamless_route =
        std::any_of(plan.headers.begin(), plan.headers.end(), [](const auto& header) {
            return HeaderNameMatches(header.first, "X-ShadPS4-Bloodborne-Seamless");
        });
    if (seamless_route && IsBloodborneSummonCreatePath(plan.path) &&
        !EnvFlagEnabled("SHADPS4_BBSC_NO_FAST_SIGN")) {
        const auto delivered = [](const HttpResponse& res) {
            const std::string_view body{reinterpret_cast<const char*>(res.body.data()),
                                        res.body.size()};
            return body.find("\"SessionId\"") != std::string_view::npos;
        };
        constexpr int FastSignTries = 5;
        constexpr auto FastSignPause = std::chrono::seconds(3);
        for (int attempt = 1; attempt <= FastSignTries; ++attempt) {
            if (local_res.status_code != 200 || delivered(local_res) || is_abandoned()) {
                break;
            }
            std::this_thread::sleep_for(FastSignPause);
            SendRequestPlan again = plan;
            RewriteConnectFirstAreaId(again);
            HttpResponse next_res;
            u32 next_bits = 0;
            const s32 next_errno = RunRealHttpRequest(again, next_res, next_bits);
            if (next_errno != 0) {
                LOG_INFO(Lib_Http, "BBSC fast sign: re-post {} failed ({:#x}), keeping the "
                                   "previous reply", attempt, static_cast<u32>(next_errno));
                break;
            }
            absorb_placement(next_res);
            local_res = std::move(next_res);
            success_event_bits = next_bits;
            LOG_INFO(Lib_Http, "BBSC fast sign: re-post {} -> status {}, {} bytes{}", attempt,
                     local_res.status_code, local_res.body.size(),
                     delivered(local_res) ? " (claim delivered)" : "");
        }
    }
}

bool IsForced() {
    return EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP") || EnvFlagEnabled("SHADPS4_CAPTURE_BLOODBORNE_SUMMON");
}

const bool s_is_registered = PsNetTitle::RegisterSummon({
    .sz_route = InheritedSummonRoute.data(),
    .pfn_is_forced = IsForced,
    .pfn_prepare = [](SendRequestPlan& plan) { ApplyBloodborneSeamlessRoute(plan); },
    .pfn_before_send = ClearStaleSeamlessSummonRecord,
    .pfn_answered = Answered,
    .pfn_finished = CaptureBloodborneSummon,
});

} // namespace
