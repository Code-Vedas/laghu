// SPDX-License-Identifier: AGPL-3.0-only
#include <openssl/ssl.h>

#ifdef LIBRESSL_VERSION_NUMBER
#error "Laghu OpenSSL probe must not accept LibreSSL"
#endif

int main() { return OpenSSL_version_num() == 0U || TLS_method() == nullptr; }
