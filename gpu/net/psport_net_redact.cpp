// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: what of an HTTP request its logs may show (kb #1321).
#include "psport_net_redact.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fmt/format.h>

namespace PsNetRedact {

namespace {

constexpr std::string_view NULL_TEXT = "(null)";

constexpr std::array<std::string_view, 21> HARMLESS_HEADERS = {
    "Accept",         "Accept-Charset",   "Accept-Encoding",   "Accept-Language", "Cache-Control",
    "Connection",     "Content-Encoding", "Content-Language",  "Content-Length",  "Content-Range",
    "Content-Type",   "Date",             "Expect",            "Host",            "Keep-Alive",
    "Pragma",         "Range",            "TE",                "Transfer-Encoding", "Upgrade",
    "User-Agent",
};

constexpr std::array<std::string_view, 4> URL_HEADERS = {"Location", "Content-Location", "Referer",
                                                         "Origin"};

bool IsSameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

bool IsListed(std::string_view name, const auto& names) {
    return std::any_of(names.begin(), names.end(),
                       [&](std::string_view listed) { return IsSameName(name, listed); });
}

std::string_view Trim(std::string_view text) {
    const auto i_first = text.find_first_not_of(" \t");
    if (i_first == std::string_view::npos) {
        return {};
    }
    return text.substr(i_first, text.find_last_not_of(" \t") - i_first + 1);
}

} // namespace

std::string HeaderValue(std::string_view name, std::string_view value) {
    if (IsListed(name, HARMLESS_HEADERS)) {
        return std::string(value);
    }
    if (IsListed(name, URL_HEADERS)) {
        return Url(value);
    }
    return fmt::format("<redacted {} bytes>", value.size());
}

std::string HeaderValue(const char* sz_name, const char* sz_value) {
    if (!sz_value) {
        return std::string(NULL_TEXT);
    }
    return HeaderValue(sz_name ? std::string_view(sz_name) : std::string_view(), sz_value);
}

std::string Url(std::string_view url) {
    std::string out;
    std::string_view rest = url;
    // A "://" inside the path or query (a redirect target as a parameter) is not this URL's scheme.
    auto off_scheme_end = url.find("://");
    if (off_scheme_end != std::string_view::npos && off_scheme_end > url.find_first_of("/?#")) {
        off_scheme_end = std::string_view::npos;
    }
    const bool has_authority = off_scheme_end != std::string_view::npos || url.starts_with("//");
    if (has_authority) {
        const auto off_authority =
            off_scheme_end == std::string_view::npos ? 2 : off_scheme_end + 3;
        out.append(url.substr(0, off_authority));
        rest = url.substr(off_authority);
        const auto cb_authority = std::min(rest.find_first_of("/?#"), rest.size());
        std::string_view authority = rest.substr(0, cb_authority);
        if (const auto off_at = authority.rfind('@'); off_at != std::string_view::npos) {
            out.append("<redacted>@");
            authority.remove_prefix(off_at + 1);
        }
        out.append(authority);
        rest.remove_prefix(cb_authority);
    }
    const auto off_query = rest.find_first_of("?#");
    out.append(rest.substr(0, off_query));
    if (off_query != std::string_view::npos) {
        out.append("?<redacted>");
    }
    return out;
}

std::string Url(const char* sz_url) {
    return sz_url ? Url(std::string_view(sz_url)) : std::string(NULL_TEXT);
}

std::string Cookie(std::string_view cookie) {
    std::string names;
    std::string_view rest = cookie;
    while (!rest.empty()) {
        const auto cb_pair = std::min(rest.find(';'), rest.size());
        const auto off_equals = rest.find('=');
        // A part without '=' is either a flag (Secure, HttpOnly) or a bare value, which may be
        // the session itself, so only what stands before an '=' counts as a name.
        const std::string_view name =
            off_equals < cb_pair ? Trim(rest.substr(0, off_equals)) : std::string_view();
        if (!name.empty()) {
            if (!names.empty()) {
                names.append(", ");
            }
            names.append(name);
        }
        rest.remove_prefix(std::min(cb_pair + 1, rest.size()));
    }
    return fmt::format("<names: {}; {} bytes>", names.empty() ? "none" : names, cookie.size());
}

std::string Cookie(const char* sz_cookie) {
    return sz_cookie ? Cookie(std::string_view(sz_cookie)) : std::string(NULL_TEXT);
}

} // namespace PsNetRedact
