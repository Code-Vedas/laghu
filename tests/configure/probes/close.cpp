// SPDX-License-Identifier: AGPL-3.0-only
#include <unistd.h>
int main() { return ::close(-1) == 0; }
