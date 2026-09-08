// SPDX-License-Identifier: AGPL-3.0-only
#include <netinet/in.h>
#include <sys/socket.h>
int main() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  (void)::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  (void)::listen(fd, 1);
  (void)::accept(fd, nullptr, nullptr);
  return 0;
}
