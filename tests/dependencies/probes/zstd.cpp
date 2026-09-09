// SPDX-License-Identifier: AGPL-3.0-only
#include <zstd.h>

int main() { return ZSTD_versionNumber() == 0U; }
