// SPDX-License-Identifier: AGPL-3.0-only
#include <sys/socket.h>
int main() { return ::socket(AF_INET, SOCK_STREAM, 0) < 0; }
