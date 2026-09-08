// SPDX-License-Identifier: AGPL-3.0-only
#include <ctime>
int main() { timespec value{}; return ::clock_gettime(CLOCK_MONOTONIC, &value); }
