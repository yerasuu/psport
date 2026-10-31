// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_skip_draws.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace PsSkipDraws {
namespace {

constexpr std::size_t C_RULES_MAX = 64;

struct Rule {
    std::uint64_t vs_hash;
    std::uint64_t ps_hash;
    bool is_any_vs;
    bool is_any_ps;
    bool has_nth;
    std::uint32_t i_nth;
    std::uint32_t c_frame_matches;
    std::atomic<std::uint64_t> c_skipped;
    std::string text;
};

struct Pair {
    std::uint64_t vs_hash;
    std::uint64_t ps_hash;
    bool operator==(const Pair&) const = default;
};

struct PairHasher {
    std::size_t operator()(const Pair& pair) const {
        return std::size_t(pair.vs_hash * 0x9e3779b97f4a7c15ull ^ pair.ps_hash);
    }
};

// Recording thread only (the draw pipe drains before another thread records a draw).
std::array<Rule, C_RULES_MAX> s_rules{};
std::size_t s_c_rules = 0;
std::unordered_map<Pair, std::uint32_t, PairHasher> s_pair_draws;
bool s_is_last_display = false;

// Read by the summary at exit.
std::mutex s_mutex;
std::unordered_map<Pair, std::uint64_t, PairHasher> s_skipped_pairs;

std::string_view Trim(std::string_view text) {
    const auto i_first = text.find_first_not_of(" \t");
    if (i_first == std::string_view::npos) {
        return {};
    }
    return text.substr(i_first, text.find_last_not_of(" \t") - i_first + 1);
}

/// Null on success, else why `text` is not a hash.
const char* ParseHash(std::string_view text, std::uint64_t& out_hash, bool& out_is_any) {
    out_hash = 0;
    out_is_any = text == "*";
    if (out_is_any) {
        return nullptr;
    }
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return "a shader hash is empty";
    }
    if (text.size() > 16) {
        return "a shader hash has more than 16 hexadecimal digits";
    }
    const char* p_end = text.data() + text.size();
    const auto [p_parsed, ec] = std::from_chars(text.data(), p_end, out_hash, 16);
    if (ec != std::errc{} || p_parsed != p_end) {
        return "a shader hash is not hexadecimal (or *)";
    }
    return nullptr;
}

/// Null on success, else why `entry` was rejected.
const char* ParseEntry(std::string_view entry, Rule& out_rule) {
    if (entry.empty()) {
        return "empty entry";
    }
    std::string_view pair = entry;
    out_rule.has_nth = false;
    out_rule.i_nth = 0;
    if (const auto i_hash = entry.find('#'); i_hash != std::string_view::npos) {
        pair = entry.substr(0, i_hash);
        const std::string_view nth = entry.substr(i_hash + 1);
        const char* p_end = nth.data() + nth.size();
        const auto [p_parsed, ec] = std::from_chars(nth.data(), p_end, out_rule.i_nth, 10);
        if (nth.empty() || ec != std::errc{} || p_parsed != p_end) {
            return "#<n> is not a decimal draw index below 2^32";
        }
        out_rule.has_nth = true;
    }
    const auto i_colon = pair.find(':');
    if (i_colon == std::string_view::npos || pair.find(':', i_colon + 1) != std::string_view::npos) {
        return "not <vs>:<ps>";
    }
    if (const char* sz_why = ParseHash(pair.substr(0, i_colon), out_rule.vs_hash, out_rule.is_any_vs)) {
        return sz_why;
    }
    if (const char* sz_why = ParseHash(pair.substr(i_colon + 1), out_rule.ps_hash, out_rule.is_any_ps)) {
        return sz_why;
    }
    const auto side = [](bool is_any, std::uint64_t hash) {
        return is_any ? std::string{"*"} : fmt::format("{:016x}", hash);
    };
    out_rule.text = side(out_rule.is_any_vs, out_rule.vs_hash) + ":" + side(out_rule.is_any_ps, out_rule.ps_hash);
    if (out_rule.has_nth) {
        out_rule.text += fmt::format("#{}", out_rule.i_nth);
    }
    return nullptr;
}

void Summarize(char* sz_out, std::size_t cb_out) {
    std::unique_lock lock{s_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        std::snprintf(sz_out, cb_out, "Skip draws: summary: busy at exit\n");
        return;
    }
    std::vector<std::pair<Pair, std::uint64_t>> pairs(s_skipped_pairs.begin(), s_skipped_pairs.end());
    std::ranges::sort(pairs, [](const auto& left, const auto& right) { return left.second > right.second; });
    std::uint64_t c_skipped = 0;
    for (const auto& [pair, c_draws] : pairs) {
        c_skipped += c_draws;
    }
    std::string text = fmt::format("Skip draws: summary: PS_SKIP_DRAWS skipped {} draws of {} pairs:", c_skipped,
                                   pairs.size());
    for (const auto& [pair, c_draws] : pairs) {
        text += fmt::format(" {:016x}:{:016x} {}", pair.vs_hash, pair.ps_hash, c_draws);
    }
    text += "; by entry:";
    for (std::size_t i = 0; i < s_c_rules; ++i) {
        text += fmt::format(" {} {}", s_rules[i].text, s_rules[i].c_skipped.load(std::memory_order_relaxed));
    }
    if (text.size() + 2 > cb_out && cb_out > 4) {
        text.resize(cb_out - 5);
        text += "...";
    }
    std::snprintf(sz_out, cb_out, "%s\n", text.c_str());
}

void Parse() {
    const char* sz_env = std::getenv("PS_SKIP_DRAWS");
    if (!sz_env || !sz_env[0]) {
        return;
    }
    runtime_calls_add_summary(&Summarize);
    std::string_view rest = sz_env;
    std::string in_force;
    while (true) {
        const auto i_comma = rest.find(',');
        const std::string_view entry = Trim(rest.substr(0, i_comma));
        if (s_c_rules == C_RULES_MAX) {
            LOG_ERROR(Render_Vulkan, "Debug: PS_SKIP_DRAWS entry '{}' ignored: more than {} entries", entry,
                      C_RULES_MAX);
        } else if (const char* sz_why = ParseEntry(entry, s_rules[s_c_rules])) {
            LOG_ERROR(Render_Vulkan, "Debug: PS_SKIP_DRAWS entry '{}' ignored: {}", entry, sz_why);
        } else {
            in_force += " " + s_rules[s_c_rules++].text;
        }
        if (i_comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(i_comma + 1);
    }
    if (s_c_rules == 0) {
        LOG_ERROR(Render_Vulkan, "Debug: PS_SKIP_DRAWS has no valid entry: no draw is skipped");
        return;
    }
    LOG_WARNING(Render_Vulkan, "Debug: PS_SKIP_DRAWS: {} entries in force:{}", s_c_rules, in_force);
}

void NoteSkip(std::uint64_t vs_hash, std::uint64_t ps_hash) {
    bool is_first = false;
    {
        std::scoped_lock lock{s_mutex};
        is_first = s_skipped_pairs[{vs_hash, ps_hash}]++ == 0;
    }
    if (is_first) {
        LOG_WARNING(Render_Vulkan, "Debug: PS_SKIP_DRAWS skipping {:016x}:{:016x}", vs_hash, ps_hash);
    }
}

} // namespace

Verdict Check(std::uint64_t vs_hash, std::uint64_t ps_hash, bool is_display_target) {
    static const bool s_is_parsed = (Parse(), true);
    (void)s_is_parsed;
    if (is_display_target && !s_is_last_display) {
        s_pair_draws.clear();
        for (std::size_t i = 0; i < s_c_rules; ++i) {
            s_rules[i].c_frame_matches = 0;
        }
    }
    s_is_last_display = is_display_target;
    const std::uint32_t i_pair_draw = s_pair_draws[{vs_hash, ps_hash}]++;
    bool is_skipped = false;
    for (std::size_t i = 0; i < s_c_rules; ++i) {
        Rule& rule = s_rules[i];
        if ((!rule.is_any_vs && rule.vs_hash != vs_hash) || (!rule.is_any_ps && rule.ps_hash != ps_hash)) {
            continue;
        }
        const std::uint32_t i_match = rule.c_frame_matches++;
        if (rule.has_nth && i_match != rule.i_nth) {
            continue;
        }
        rule.c_skipped.fetch_add(1, std::memory_order_relaxed);
        is_skipped = true;
    }
    if (is_skipped) {
        NoteSkip(vs_hash, ps_hash);
    }
    return {is_skipped, i_pair_draw};
}

} // namespace PsSkipDraws
