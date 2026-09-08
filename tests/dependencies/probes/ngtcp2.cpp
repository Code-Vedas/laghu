// SPDX-License-Identifier: AGPL-3.0-only
#include <ngtcp2/ngtcp2.h>

int main() { return ngtcp2_version(0) == nullptr; }
