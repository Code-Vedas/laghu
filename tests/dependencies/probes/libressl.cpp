// SPDX-License-Identifier: AGPL-3.0-only
#include <openssl/ssl.h>

#ifndef LIBRESSL_VERSION_NUMBER
#error "Laghu LibreSSL probe requires LibreSSL"
#endif

int main() { return OpenSSL_version_num() == 0U || TLS_method() == nullptr; }
