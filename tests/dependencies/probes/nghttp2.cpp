// SPDX-License-Identifier: AGPL-3.0-only
#include <nghttp2/nghttp2.h>

int main() { return nghttp2_version(0) == nullptr; }
