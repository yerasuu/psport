// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: what of an HTTP request its logs may show (kb #1321). Run logs reach
// failures.jsonl, report attachments and the clipboard, so header values, cookies and URL queries
// (bearer tokens, session cookies, signed queries) never go into them.
#pragma once
#include <string>
#include <string_view>

namespace PsNetRedact {

/// A header's value as a log line may show it: the value itself for the headers that never carry
/// credentials (Content-Type, Content-Length, Accept, User-Agent, Host...), the URL rule for the
/// ones holding a URL (Location, Referer, Origin), `<redacted N bytes>` for every other name.
/// The name itself is always safe to log.
std::string HeaderValue(std::string_view name, std::string_view value);
std::string HeaderValue(const char* sz_name, const char* sz_value);

/// A URL or request target as `scheme://host[:port]/path`: the user info becomes `<redacted>@`,
/// the query and fragment `?<redacted>`. A target without a scheme keeps its path alone.
std::string Url(std::string_view url);
std::string Url(const char* sz_url);

/// A cookie string as its names and length only: `<names: a, b; 42 bytes>`.
std::string Cookie(std::string_view cookie);
std::string Cookie(const char* sz_cookie);

} // namespace PsNetRedact
