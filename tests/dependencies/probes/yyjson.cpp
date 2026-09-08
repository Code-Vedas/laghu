// SPDX-License-Identifier: AGPL-3.0-only
#include <yyjson.h>

int main() { return yyjson_read_opts("{}", 2U, YYJSON_READ_NOFLAG, nullptr, nullptr) == nullptr; }
