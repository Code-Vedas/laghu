// SPDX-License-Identifier: AGPL-3.0-only
#include <poll.h>
int main() { pollfd descriptor{}; return ::poll(&descriptor, 1, 0) < 0; }
