// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// psport: the request libSceHttp sends and what it gets back, shared with a title's summon client
// (psport_net_title.h), which rewrites, repeats and records its own calls.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/libraries/network/http.h"
#include "http_error.h"

namespace Libraries::Http {

struct HttpSettings {
    u32 connect_timeout_us = 0;
    u32 send_timeout_us = 0;
    u32 recv_timeout_us = 0;
    u32 resolve_timeout_us = 0;  // DNS resolution timeout (sceHttpSetResolveTimeOut)
    s32 resolve_retry = 0;       // DNS retry count (sceHttpSetResolveRetry)
    u64 recv_block_size = 0;     // Hint to streaming receiver; 0 = default
    u64 response_header_max = 0; // Max response-header bytes accepted; 0 = default
    bool auto_redirect = true;
    bool inflate_gzip = true;                     // Auto-decompress response body if gzipped
    bool accept_encoding_gzip = true;             // Send "Accept-Encoding: gzip" request header
    u32 ssl_flags = ORBIS_HTTPS_FLAG_SDK_DEFAULT; // SSL flag mask. Bitmask of OrbisHttpsFlags.
    bool nonblock = false; // false = blocking (default), true = nonblock (EAGAIN)
    // (0 = no proxy, 1 = manual host:port, 2 = automatic/PAC).
    std::string proxy_host;
    u16 proxy_port = 0;
    int proxy_http_conf = 0;
    int proxy_wlan_conf = 0;
};

struct HttpResponse {
    int status_code = 0;
    u64 content_length = 0;
    int content_length_result = ORBIS_HTTP_ERROR_NO_CONTENT_LENGTH;
    std::vector<u8> body;
    u64 read_cursor = 0;
    std::string all_headers_blob; // Pre-formatted "Name: Value\r\n..." string.
};

struct HostOverrideTarget {
    std::string scheme; // "http", "https", or "" to mean "preserve original"
    std::string host;
    u16 port = 0; // 0 means "preserve original (or default-for-scheme if scheme changed)"
};

struct SendRequestPlan {
    std::string scheme;
    std::string host;
    u16 port = 0;
    std::string path; // Path + query, e.g. "/utility/time" or "/?x=1"
    s32 method = 0;
    std::string method_str; // Only used when method == ORBIS_HTTP_METHOD_CUSTOM
    std::string content_type;
    std::vector<u8> body;
    std::vector<std::pair<std::string, std::string>> headers; // Merged tmpl + conn + req
    HttpSettings settings;
    // True if the request's owning ctx had sceHttpsLoadCert called. In that
    // case we bypass TLS verification because we can't load the game's CAs.
    bool ctx_has_loaded_certs = false;
};

bool HeaderNameMatches(std::string_view a, std::string_view b);
const char* HttpMethodName(s32 method);
HostOverrideTarget ParseHostOverrideTarget(std::string_view value);
/// Sends plan as it stands, its redirects followed: 0 or an ORBIS_HTTP_ERROR code.
s32 RunRealHttpRequest(const SendRequestPlan& plan_in, HttpResponse& out_res, u32& out_event_bits);

} // namespace Libraries::Http
