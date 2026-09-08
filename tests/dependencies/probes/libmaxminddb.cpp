// SPDX-License-Identifier: AGPL-3.0-only
#include <maxminddb.h>

int main() {
  MMDB_s mmdb{};
  MMDB_close(&mmdb);
  return 0;
}
