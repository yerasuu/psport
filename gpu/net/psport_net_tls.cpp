// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: the shadNet connection's TLS stream and login policy (kb #1297) and its
// WebAPI's (kb #1302).
#include "psport_net_tls.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <string_view>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/emulator_settings.h"

namespace PsNetTls {

namespace {

namespace tls = httplib::tls;

constexpr std::uint32_t WRITE_WAIT_MS = 10000;
constexpr std::uint16_t WEBAPI_PORT = 31315;

bool IsLoopback(std::uint32_t va_peer_ipv4) {
    return (ntohl(va_peer_ipv4) >> 24) == 127;
}

bool IsPowerOfTwo(std::uint64_t c_value) {
    return (c_value & (c_value - 1)) == 0;
}

std::string Lowercase(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

struct BaseUrl {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    bool has_scheme = false;
    bool has_path = false;
};

// As cpp-httplib's Client reads a base URL, where no scheme means http. User info is refused, so
// that no credential in the address reaches a log line.
std::optional<BaseUrl> ParseBaseUrl(std::string_view url) {
    BaseUrl parsed;
    parsed.scheme = "http";
    if (const auto off_scheme = url.find("://"); off_scheme != std::string_view::npos) {
        parsed.scheme = Lowercase(url.substr(0, off_scheme));
        parsed.has_scheme = true;
        url.remove_prefix(off_scheme + 3);
    }
    if (parsed.scheme != "http" && parsed.scheme != "https")
        return std::nullopt;
    const auto off_path = url.find_first_of("/?#");
    const std::string_view authority = url.substr(0, off_path);
    parsed.has_path = off_path != std::string_view::npos && url.substr(off_path) != "/";
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return std::nullopt;
    std::string_view port_text;
    if (authority.front() == '[') {
        const auto off_close = authority.find(']');
        if (off_close == std::string_view::npos)
            return std::nullopt;
        parsed.host = Lowercase(authority.substr(1, off_close - 1));
        const std::string_view after = authority.substr(off_close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return std::nullopt;
            port_text = after.substr(1);
        }
    } else if (const auto off_colon = authority.rfind(':'); off_colon != std::string_view::npos) {
        parsed.host = Lowercase(authority.substr(0, off_colon));
        port_text = authority.substr(off_colon + 1);
    } else {
        parsed.host = Lowercase(authority);
    }
    if (parsed.host.empty())
        return std::nullopt;
    parsed.port = parsed.scheme == "https" ? 443 : 80;
    if (!port_text.empty()) {
        unsigned value = 0;
        const char* p_end = port_text.data() + port_text.size();
        const auto [p_parsed, error] = std::from_chars(port_text.data(), p_end, value);
        if (error != std::errc{} || p_parsed != p_end || value == 0 || value > 0xFFFF)
            return std::nullopt;
        parsed.port = static_cast<std::uint16_t>(value);
    }
    return parsed;
}

// run.sh (${PS_SHADNET_SERVER%:*}) and the launchers always pass the WebAPI they derive from the
// shadNet server, so the address alone tells a derived one from one set apart.
bool IsDerivedWebApi(const BaseUrl& parsed) {
    const std::string server = EmulatorSettings.GetShadNetServer();
    const auto off_colon = server.rfind(':');
    const std::string server_host =
        Lowercase(off_colon == std::string::npos ? server : server.substr(0, off_colon));
    return parsed.has_scheme && parsed.scheme == "http" && !parsed.has_path &&
           parsed.port == WEBAPI_PORT && parsed.host == server_host;
}

struct WebApiTarget {
    bool is_valid = false;
    bool is_https = false;
    std::string host;
    std::uint16_t port = 0;
    std::string url;
};

const WebApiTarget& GetWebApiTarget() {
    static const WebApiTarget s_target = [] {
        WebApiTarget target;
        const std::string url = EmulatorSettings.GetShadNetWebApiServer();
        const auto parsed = ParseBaseUrl(url);
        if (!parsed) {
            LOG_ERROR(ShadNet, "PS_SHADNET_WEBAPI is not http(s)://host[:port][/path] without user "
                               "info; every WebAPI request is refused");
            return target;
        }
        target = {true, parsed->scheme == "https", parsed->host, parsed->port, url};
        if (target.is_https || !EmulatorSettings.IsShadNetTlsRequired())
            return target;
        if (IsDerivedWebApi(*parsed)) {
            target.is_https = true;
            target.url = "https://" + url.substr(url.find("://") + 3);
            LOG_INFO(ShadNet, "WebAPI {}:{} upgraded from http to https since PS_SHADNET_TLS=1",
                     target.host, target.port);
        } else {
            LOG_WARNING(ShadNet,
                        "WebAPI {}:{} stays http though PS_SHADNET_TLS=1: PS_SHADNET_WEBAPI names "
                        "an address apart from the shadNet server's, so it is not upgraded",
                        target.host, target.port);
        }
        return target;
    }();
    return s_target;
}

// Every address must be loopback, since cpp-httplib may connect to any of them.
bool ResolvesToLoopbackOnly(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* p_results = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &p_results) != 0 || !p_results)
        return false;
    bool is_loopback = true;
    for (const addrinfo* p_result = p_results; p_result && is_loopback;
         p_result = p_result->ai_next) {
        if (p_result->ai_family == AF_INET) {
            is_loopback = IsLoopback(
                reinterpret_cast<const sockaddr_in*>(p_result->ai_addr)->sin_addr.s_addr);
        } else if (p_result->ai_family == AF_INET6) {
            is_loopback = IN6_IS_ADDR_LOOPBACK(
                &reinterpret_cast<const sockaddr_in6*>(p_result->ai_addr)->sin6_addr);
        } else {
            is_loopback = false;
        }
    }
    ::freeaddrinfo(p_results);
    return is_loopback;
}

ssize_t WaitForSocket(socket_t h_socket, bool should_read, std::uint32_t wait_ms) {
    const time_t sec = wait_ms / 1000;
    const time_t usec = (wait_ms % 1000) * 1000;
    return should_read ? httplib::detail::select_read(h_socket, sec, usec)
                       : httplib::detail::select_write(h_socket, sec, usec);
}

} // namespace

LoginTransport ChooseLoginTransport(bool is_tls, std::uint32_t va_peer_ipv4, const std::string& host,
                                    std::uint16_t port) {
    static std::atomic<std::uint64_t> s_c_tls{0};
    static std::atomic<std::uint64_t> s_c_loopback{0};
    static std::atomic<std::uint64_t> s_c_plain_allowed{0};
    if (is_tls) {
        LOG_INFO(ShadNet, "Login to {}:{} over TLS ({} so far)", host, port, ++s_c_tls);
        return LoginTransport::Tls;
    }
    if (IsLoopback(va_peer_ipv4)) {
        LOG_INFO(ShadNet, "Login to {}:{} over plain TCP to a loopback address ({} so far)", host,
                 port, ++s_c_loopback);
        return LoginTransport::PlainLoopback;
    }
    if (EmulatorSettings.IsPlaintextLoginAllowed()) {
        LOG_WARNING(ShadNet,
                    "Login to {}:{} over plain TCP: password and token cross the network "
                    "unencrypted, allowed by PS_ONLINE_ALLOW_PLAINTEXT_LOGIN ({} so far)",
                    host, port, ++s_c_plain_allowed);
        return LoginTransport::PlainAllowed;
    }
    LOG_ERROR_COUNTED(ShadNet,
                      "Login to {}:{} refused: plain TCP to a server off this machine would send "
                      "the password unencrypted; PS_SHADNET_TLS=1 for a TLS server, or "
                      "PS_ONLINE_ALLOW_PLAINTEXT_LOGIN=1 to accept the risk",
                      host, port);
    return LoginTransport::Refused;
}

std::optional<std::string> WebApiUrl() {
    static std::atomic<std::uint64_t> s_c_tls{0};
    static std::atomic<std::uint64_t> s_c_loopback{0};
    static std::atomic<std::uint64_t> s_c_plain_allowed{0};
    const WebApiTarget& target = GetWebApiTarget();
    if (!target.is_valid) {
        LOG_ERROR_COUNTED(ShadNet, "WebAPI request refused: PS_SHADNET_WEBAPI is not usable");
        return std::nullopt;
    }
    if (target.is_https) {
        if (const auto c_tls = ++s_c_tls; IsPowerOfTwo(c_tls))
            LOG_INFO(ShadNet, "WebAPI {}:{} over TLS ({} so far)", target.host, target.port, c_tls);
        return target.url;
    }
    if (ResolvesToLoopbackOnly(target.host)) {
        if (const auto c_loopback = ++s_c_loopback; IsPowerOfTwo(c_loopback))
            LOG_INFO(ShadNet, "WebAPI {}:{} over plain HTTP to a loopback address ({} so far)",
                     target.host, target.port, c_loopback);
        return target.url;
    }
    if (EmulatorSettings.IsPlaintextLoginAllowed()) {
        const auto c_plain = ++s_c_plain_allowed;
        if (c_plain == 1)
            LOG_WARNING(ShadNet,
                        "WebAPI {}:{} over plain HTTP: the account's bearer token crosses the "
                        "network unencrypted, allowed by PS_ONLINE_ALLOW_PLAINTEXT_LOGIN",
                        target.host, target.port);
        else if (IsPowerOfTwo(c_plain))
            LOG_INFO(ShadNet, "WebAPI {}:{} over plain HTTP ({} so far)", target.host, target.port,
                     c_plain);
        return target.url;
    }
    LOG_ERROR_COUNTED(ShadNet,
                      "WebAPI request to {}:{} refused: plain HTTP to a server off this machine "
                      "would send the bearer token unencrypted; an https PS_SHADNET_WEBAPI or "
                      "PS_SHADNET_TLS=1 for a TLS server, or PS_ONLINE_ALLOW_PLAINTEXT_LOGIN=1 to "
                      "accept the risk",
                      target.host, target.port);
    return std::nullopt;
}

bool IsWebApiEndpoint(const std::string& scheme, const std::string& host, std::uint16_t port) {
    if (!EmulatorSettings.IsConnectedToNetwork() || Lowercase(scheme) != "https")
        return false;
    const WebApiTarget& target = GetWebApiTarget();
    std::string_view bare_host = host;
    if (bare_host.size() > 2 && bare_host.front() == '[' && bare_host.back() == ']')
        bare_host = bare_host.substr(1, bare_host.size() - 2);
    return target.is_valid && target.is_https && port == target.port &&
           Lowercase(bare_host) == target.host;
}

Stream::~Stream() {
    if (m_p_session)
        tls::free_session(m_p_session);
    if (m_p_context)
        tls::free_context(m_p_context);
}

bool Stream::Connect(socket_t h_socket, const std::string& host, std::uint32_t timeout_ms,
                     std::string& out_error) {
    m_h_socket = h_socket;
    m_p_context = tls::create_client_context();
    if (!m_p_context) {
        out_error = "no TLS context (" + tls::error_string(tls::get_error()) + ")";
        return false;
    }
    bool has_roots = false;
    if (const char* sz_ca_file = std::getenv("SSL_CERT_FILE"); sz_ca_file && *sz_ca_file) {
        has_roots = tls::load_ca_file(m_p_context, sz_ca_file);
        if (!has_roots)
            LOG_WARNING(ShadNet, "TLS: SSL_CERT_FILE '{}' not readable as certificates",
                        sz_ca_file);
    }
    has_roots = tls::load_system_certs(m_p_context) || has_roots;
    if (!has_roots) {
        out_error = "no CA certificates (SSL_CERT_FILE or the system store)";
        return false;
    }
    m_p_session = tls::create_session(m_p_context, h_socket);
    if (!m_p_session) {
        out_error = "no TLS session (" + tls::error_string(tls::get_error()) + ")";
        return false;
    }
    // mbedTLS checks the certificate's names against this one (it is also the SNI); without it
    // 3.6 refuses to verify, which is the outcome we want for an empty host too.
    if (!tls::set_hostname(m_p_session, host.c_str())) {
        out_error = "host name not accepted for verification";
        return false;
    }
    tls::TlsError error;
    if (!tls::connect_nonblocking(m_p_session, h_socket, timeout_ms / 1000,
                                  (timeout_ms % 1000) * 1000, &error)) {
        if (error.code == tls::ErrorCode::CertVerifyFailed)
            out_error = "certificate rejected: " +
                        tls::verify_error_string(tls::get_verify_result(m_p_session));
        else
            out_error = fmt::format("handshake failed or timed out ({})",
                                    tls::error_string(error.backend_code));
        return false;
    }
    if (const long verify = tls::get_verify_result(m_p_session); verify != 0) {
        out_error = "certificate rejected: " + tls::verify_error_string(verify);
        return false;
    }
    httplib::detail::set_nonblocking(h_socket, true);
    return true;
}

Stream::ReadResult Stream::ReadSome(std::uint8_t* p_buffer, std::size_t cb_buffer,
                                    std::uint32_t wait_ms, std::size_t& cb_out_read) {
    cb_out_read = 0;
    for (;;) {
        tls::TlsError error;
        ssize_t cb_read;
        {
            std::lock_guard lock(m_mutex);
            cb_read = tls::read(m_p_session, p_buffer, cb_buffer, error);
        }
        if (cb_read > 0) {
            cb_out_read = static_cast<std::size_t>(cb_read);
            return ReadResult::Ok;
        }
        if (cb_read == 0)
            return ReadResult::Closed;
        bool should_read = true;
        switch (error.code) {
        case tls::ErrorCode::WantRead:
            break;
        case tls::ErrorCode::WantWrite:
            should_read = false;
            break;
        default:
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            // mbedTLS 3.6.0 hands a TLS 1.3 session ticket to the caller as an error; the stream
            // goes on after it.
            if (error.backend_code ==
                static_cast<std::uint64_t>(-MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET))
                continue;
#endif
            LOG_WARNING(ShadNet, "TLS read failed ({})", tls::error_string(error.backend_code));
            return ReadResult::Failed;
        }
        const ssize_t c_ready = WaitForSocket(m_h_socket, should_read, wait_ms);
        if (c_ready == 0)
            return ReadResult::TimedOut;
        if (c_ready < 0)
            return ReadResult::Failed;
    }
}

bool Stream::ReadExact(std::uint8_t* p_buffer, std::size_t cb_buffer, std::uint32_t timeout_ms) {
    std::size_t cb_done = 0;
    while (cb_done < cb_buffer) {
        std::size_t cb_read = 0;
        if (ReadSome(p_buffer + cb_done, cb_buffer - cb_done, timeout_ms, cb_read) !=
            ReadResult::Ok)
            return false;
        cb_done += cb_read;
    }
    return true;
}

bool Stream::WriteAll(const std::uint8_t* p_data, std::size_t cb_data) {
    // The lock is held across the waits too, since mbedTLS wants a write it answered WANT_WRITE
    // repeated before any other call on the session.
    std::lock_guard lock(m_mutex);
    std::size_t cb_sent = 0;
    while (cb_sent < cb_data) {
        tls::TlsError error;
        const ssize_t cb_written =
            tls::write(m_p_session, p_data + cb_sent, cb_data - cb_sent, error);
        if (cb_written > 0) {
            cb_sent += static_cast<std::size_t>(cb_written);
            continue;
        }
        if (error.code != tls::ErrorCode::WantWrite && error.code != tls::ErrorCode::WantRead) {
            LOG_WARNING(ShadNet, "TLS write failed ({})", tls::error_string(error.backend_code));
            return false;
        }
        if (WaitForSocket(m_h_socket, error.code == tls::ErrorCode::WantRead, WRITE_WAIT_MS) <= 0) {
            LOG_WARNING(ShadNet, "TLS write: socket not ready within {} ms", WRITE_WAIT_MS);
            return false;
        }
    }
    return true;
}

} // namespace PsNetTls
