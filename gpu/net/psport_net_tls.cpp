// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: the shadNet connection's TLS stream and login policy (kb #1297).
#include "psport_net_tls.h"

#include <atomic>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/emulator_settings.h"

namespace PsNetTls {

namespace {

namespace tls = httplib::tls;

constexpr std::uint32_t WRITE_WAIT_MS = 10000;

bool IsLoopback(std::uint32_t va_peer_ipv4) {
    return (ntohl(va_peer_ipv4) >> 24) == 127;
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
