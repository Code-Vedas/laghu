// SPDX-License-Identifier: AGPL-3.0-only
#include <fcntl.h>
int main() { return ::fcntl(0, F_SETFL, O_NONBLOCK) < 0; }
