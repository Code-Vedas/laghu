// SPDX-License-Identifier: AGPL-3.0-only
#include <unistd.h>
#if defined(LAGHU_POSIX_REQUIRE_NUMERIC) && (!defined(_POSIX_VERSION) || _POSIX_VERSION < 200809L)
#error "Laghu requires POSIX.1-2008 or later"
#endif
int main() { return 0; }
