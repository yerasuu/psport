// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: the CA bundle of its HTTPS clients (cpp-httplib over mbedTLS, kb #850).
#pragma once
#include <cstdlib>

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

} // namespace PsNetTls
