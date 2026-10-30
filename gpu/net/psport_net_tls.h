// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: the CA bundle of its HTTPS clients (cpp-httplib over mbedTLS, kb #850) and
// the shadNet connection's TLS and login policy (kb #1297).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

#include <httplib.h>

namespace PsNetTls {

/// mbedTLS, unlike OpenSSL, reads no SSL_CERT_FILE: cpp-httplib then looks only in the usual
/// system paths (/etc/ssl/certs/...), which the AppImage on NixOS cannot follow into the host's
/// store. run.sh and the package wrappers name a readable bundle in SSL_CERT_FILE; it replaces the
/// system roots as it did with OpenSSL. Without it the client keeps cpp-httplib's own search.
inline void UseCaBundle(httplib::Client& client) {
    if (const char* sz_ca_file = std::getenv("SSL_CERT_FILE"); sz_ca_file && *sz_ca_file) {
        client.set_ca_cert_path(sz_ca_file);
    }
}

enum class LoginTransport { Tls, PlainLoopback, PlainAllowed, Refused };

/// Whether the login (password, token) may go over the connection just made: always over TLS
/// (PS_SHADNET_TLS=1, no fallback to plain TCP); over plain TCP only to a loopback peer
/// (va_peer_ipv4 in network order) or with PS_ONLINE_ALLOW_PLAINTEXT_LOGIN=1. Counts and logs the
/// choice.
LoginTransport ChooseLoginTransport(bool is_tls, std::uint32_t va_peer_ipv4, const std::string& host,
                                    std::uint16_t port);

/// A TLS client stream over a connected TCP socket the caller owns and closes. One reader and one
/// writer thread may use it at once: an mbedTLS context is not thread-safe, so every call into the
/// session holds m_mutex and the reader waits for data outside it.
class Stream {
public:
    enum class ReadResult { Ok, TimedOut, Closed, Failed };

    Stream() = default;
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /// Handshake with full certificate verification; on failure out_error says why. Leaves the
    /// socket non-blocking.
    bool Connect(socket_t h_socket, const std::string& host, std::uint32_t timeout_ms,
                 std::string& out_error);
    ReadResult ReadSome(std::uint8_t* p_buffer, std::size_t cb_buffer, std::uint32_t wait_ms,
                        std::size_t& cb_out_read);
    /// Fails on a close, an error or timeout_ms without a byte.
    bool ReadExact(std::uint8_t* p_buffer, std::size_t cb_buffer, std::uint32_t timeout_ms);
    bool WriteAll(const std::uint8_t* p_data, std::size_t cb_data);

private:
    std::mutex m_mutex;
    socket_t m_h_socket = INVALID_SOCKET;
    httplib::tls::ctx_t m_p_context = nullptr;
    httplib::tls::session_t m_p_session = nullptr;
};

} // namespace PsNetTls
