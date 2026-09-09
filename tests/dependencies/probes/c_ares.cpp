// SPDX-License-Identifier: AGPL-3.0-only
#include <ares.h>

int main() { return ares_version(nullptr) == nullptr; }
